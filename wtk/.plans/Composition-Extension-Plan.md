# Composition Extension Plan

Consolidated plan for extending the Brush, Color, and Gradient APIs in `omegaWTK/Composition`, plus the brush/pipeline and text-layout work that rides the FrameBuilder / `DrawOp` compositor model. Supersedes the earlier separate extension proposals.

**Compositor backend assumed by this plan:** the
Direct-To-Drawable / SDF backend
(see `Direct-To-Drawable-And-SDF-Plan.md`) is in for the simple
primitives. Concrete implications for this plan:

  - `Brush::Type::Color` fills on Rect / RoundedRect / Ellipse /
    Shadow run through the SDF pipeline (one 6-vertex quad per
    primitive, closed-form distance evaluation).
  - The optional `Border` slot on Rect / RoundedRect / Ellipse is
    consumed by the SDF fragment shader — fill + stroke are produced
    by one draw call, no separate stroked-path emission.
  - `Brush::Type::Gradient` still falls back to the tessellation +
    texture pipeline (Phase 1's gradient pipeline work below). The
    SDF shader does not yet sample a gradient texture — that's a
    Phase 1 / Phase 2 follow-up to extend the SDF fragment with a
    gradient sampler when the brush type is gradient.
  - The triangulator is opened lazily (only on `VectorPath`,
    `Bitmap`, or gradient-fallback draws).

These behaviors shape the work below — Phase 1 needs a gradient
texture, but Phase 1 also has the option to skip the texture entirely
by extending the SDF fragment shader to compute gradient colors
analytically. Both routes are noted where relevant.

---

## Compositor op model: FrameBuilder + `DisplayList<DrawOp>`

The compositor records one `DisplayList<DrawOp>` per window per frame:
every view appends to it via the `FrameBuilder` (there is no per-view
paint device). `DrawOp` is the single compositor op type — one record
per primitive, fill + border consolidated, soft shadow as its own SDF
op — and the backend `BackendRenderTargetContext::renderToTarget`
switch dispatches on `DrawOp`. Draw state (transform / opacity / clip)
rides the `DisplayList` itself via `SetTransform` / `SetOpacity` and
`PushClip` / `PopClip` / `PushTransform` / `PopTransform` ops, so
imperative line / polyline / arc / path draws are just `DrawOp::VectorPath`
payload shapes and per-draw opacity / blend is a field on the relevant
op or a `PushOpacity` / `PushEffect` scope.

**Why the brush / pipeline work below is op-type-agnostic.** Backend
rasterization reads *payload structs* (brush, rect, border, gradient
params, texture handle), not the op wrapper. The SDF pipeline, the
tessellation + texture pipeline, the gradient compute pass, the bitmap
blit, and the MSDF text path all consume those payloads — so the
gradient, bitmap-brush, glass, and text work in this plan lives in the
brush model, the shader source, and `RenderTarget.cpp`'s rasterization,
independent of the op type. Author against the payload.

The phases that matter going forward are the **brush / pipeline** ones:
gradients (Phase 1 / 2, consolidated by Phase 9), the new **bitmap
brushes** (Phase 8), **glass brushes** (Phase 11), MSDF scalable
bitmaps (Phase 10), and the **text-layout reuse** work (Phase 6).

---

## Current state snapshot

### What works

- `Brush::Type` enum (`Color`, `Gradient`, `None`) is the single dispatch source. Legacy `isColor`/`isGradient` booleans are gone (Phase 0 done).
- `GradientBrush()` header/source signature mismatch is fixed.
- `create32Bit` typo is fixed.
- Shape `DrawOp`s (Rect / RoundedRect / Ellipse) carry an optional color `Border`.
- Color solid-fill rendering works for all shape primitives and paths.
- `Path` supports `addArc`, `addLine`, `goTo`, `close`, stroke width, and a per-path brush.
- **`Composition/Geometry.h`** owns the public 2D geometry vocabulary; `Core/Core.h` no longer includes `<OmegaGTE.h>`; `Core/GTEHandle.h` is the backend-only handle (Phase 0A done).
- **Border consolidation:** Rect / RoundedRect / Ellipse with a color border emit one `DrawOp` and render via one SDF draw — the prior `RectFrame` / `RoundedRectFrame` / `EllipseFrame` side-emission is gone (Direct-To-Drawable-And-SDF-Plan §6.5). The frame helpers remain in `Path.h` for clients that explicitly want a stand-alone outline path.
- Per-element transform / opacity and clip ride the `DisplayList` as `SetTransform` / `SetOpacity` and `PushClip` / `PushTransform` ops.

### What doesn't work or is incomplete

| Issue | Location | Impact |
|-------|----------|--------|
| Gradient compute shader is commented out | `compositor.omegasl` (`linearGradient`), `RenderTarget.cpp` const buffer write | Gradient brushes enter the texture pipeline but produce no texture — effectively broken. SDF fragment shader does not yet sample gradient textures either |
| Gradient has only `float arg` (angle or radius) | `Brush.h:62` | No start/end points for linear, no center/focus for radial, no elliptical radii |
| No spread mode on gradients | — | Out-of-range stops clamp implicitly |
| Raw `OmegaGTE::` types still appear in `Path.h` internal fields | `Path.h` `Segment::path` (`GVectorPath2D`) | Public signatures partially migrated; some internal fields still expose the GTE types via forward decls |

---

## Phase 0 — Foundation cleanup [DONE]

**Goal:** Make `Brush::Type` the single source of truth. Remove ambiguity before adding anything new.

**Status:** Complete. Every callsite in `RenderTarget.cpp` dispatches
via `_params.brush->type` (Color / Gradient / None). The legacy
`isColor` / `isGradient` fields on `Brush` have been removed. The SDF
spine in `Direct-To-Drawable-And-SDF-Plan.md` §6.3 was authored against
this contract from the start.

### 0.1 Migrate backend to `switch(brush->type)` [DONE]

Every site in `RenderTarget.cpp` that read `brush->isColor` or
`!brush->isColor` now uses:

```cpp
if (brush->type == Brush::Type::None) return;
if (brush->type == Brush::Type::Color) { /* SDF or solid path */ return; }
// Gradient fall-through to tessellation + texture pipeline
```

Locations covered: Rect, RoundedRect, Ellipse, Shadow (all SDF after
6.3), VectorPath stroke and fill.

### 0.2 Remove legacy booleans [DONE]

`isColor` and `isGradient` are gone from `Brush.h` / `Brush.cpp`. No
out-of-tree consumers remained.

### Files touched

- `wtk/include/omegaWTK/Composition/Brush.h` — `isColor` / `isGradient` fields removed
- `wtk/src/Composition/Brush.cpp` — constructors no longer assign the booleans
- `wtk/src/Composition/backend/RenderTarget.cpp` — switch-based dispatch

---

## Phase 0A — Geometry type isolation [DONE]

**Goal:** Stop exposing `<OmegaGTE.h>` through Core and Composition public headers. Define standalone geometry wrapper structs in the Composition submodule (which owns the Compositor — the only part that actually talks to GTE) so that OmegaGTE can be linked to `OmegaWTK_Composition` only instead of the top-level `OmegaWTK` target. The exception is `OmegaGTEView`, which talks to GTE directly by design.

**Status:** Complete. `wtk/include/omegaWTK/Composition/Geometry.h`
owns the standalone PODs; `wtk/include/omegaWTK/Core/GTEHandle.h`
holds the backend-only `extern OmegaGTE::GTE gte`; `Core/Core.h` no
longer pulls in `<OmegaGTE.h>`; the ~130-file mechanical rename to
`Composition::Rect` / `Composition::Point2D` etc. landed under 0A.3a.
A few internal struct fields (`Path::Segment::path`, and the `DrawOp`
path / transform payload fields) still hold raw GTE types behind
forward declarations — that's working as intended for the
implementation-detail boundary. See "What doesn't work" in the
snapshot for the residue.

### Problem

`Core/Core.h` does `#include <OmegaGTE.h>` at the top. Every header that includes `Core.h` — which is every Composition and UI header — pulls in the entire GTE surface: vector path templates, matrix types, 3D geometry, shader pipeline types, etc. This inflates translation units across all of WTK even though most modules only need a handful of 2D geometry PODs.

Additionally, several Composition headers expose raw `OmegaGTE::` types in public signatures:

| Type | Where exposed |
|------|---------------|
| `OmegaGTE::GPoint2D` | `Path.h` — `goTo()`, `addLine()`, constructor |
| `OmegaGTE::GVectorPath2D` | `Path.h` — `Segment` fields, constructor; the path-op payload |
| `OmegaGTE::GETexture` / `OmegaGTE::GEFence` | the bitmap-op payload; `FontEngine.h` — `TextRect::BitmapRes` |
| `OmegaGTE::FMatrix<4,4>` | the transform-op payload |

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
- The internal `Segment` struct keeps `OmegaGTE::GVectorPath2D` — it's `friend class FrameBuilder` / private, not public API. But the `Path(GVectorPath2D &)` constructor should either become private/friend-only or take a WTK-level wrapper. For now, make it `private` with `friend class FrameBuilder` since it's only used internally.

**`DrawOp` payloads (`DisplayList.h`):**
- The path / bitmap / transform op payloads keep raw GTE types (`GVectorPath2D`, `GETexture` / `GEFence`, `FMatrix<4,4>`) behind forward declarations — they are backend implementation detail, not public API, so this is working as intended.

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

Every `.cpp` file in the Composition submodule that actually constructs GTE objects (the backend, `Path.cpp`, `RenderTarget.cpp`) adds `#include "GeometryConvert.h"` and converts at the WTK↔GTE boundary.

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
- `wtk/include/omegaWTK/Composition/Animation.h` — `GPoint2D` → `Composition::Point2D` in public signatures
- ~130 files across all submodules — mechanical `Composition::Rect` → `Composition::Rect` (and Position/RoundedRect/Ellipse) rename
- `wtk/src/Composition/Path.cpp` — add `GeometryConvert.h`, convert at boundary
- `wtk/src/Composition/backend/RenderTarget.cpp` — add `GeometryConvert.h` (already includes GTE)
- `wtk/CMakeLists.txt` — link OmegaGTE `PRIVATE` to `OmegaWTK_Composition`; remove `PUBLIC` GTE from `OmegaWTK` framework target

---

## Phase 1 — Gradient pipeline

**Goal:** Make `GradientBrush(...)` actually produce visible output. This is the single largest prerequisite for everything gradient-related.

**Choice point — texture path vs SDF-native:** with the SDF spine in,
there are now two viable architectures for gradient fills on simple
primitives:

  - **Texture path (the original plan).** Run a compute shader to
    rasterize the gradient into a texture, then sample it from the
    fragment shader. This is what 1.1–1.4 below describe. Works for
    simple primitives, vector paths, and bitmaps with no per-shape
    customization.
  - **SDF-native path.** Add a gradient sampler to `sdfFragment` in
    `compositor.omegasl`: pass start/end / center/radii in the
    per-draw uniform, evaluate the gradient parameter `t` from the
    interpolated local coord, look up stop colors from a small
    constant array. Avoids the compute pass and the texture
    allocation entirely for simple primitives. Vector paths with
    gradient fills still need the texture path because their
    fragment shader is `mainFragment` (color attachment-driven).

For Phase 1, implement the texture path first (1.1–1.4) — it covers
all primitives uniformly and unblocks the gradient API extensions.
The SDF-native gradient is a Phase 2 follow-up that improves the
common case once the API stabilizes.

### 1.1 Implement the gradient compute shader

The shader source `compositor.omegasl` already defines `GradientTextureConstParams` and `LinearGradientStop`, but the `linearGradient` compute function is commented out. Implement:

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

After the SDF spine, the gradient-brush rect / rounded-rect cases
explicitly fall back to the existing tessellation + texture pipeline
(see comments in `RenderTarget.cpp` around the Rect / RoundedRect
cases). This phase populates the texture binding for that fallback —
the dispatch wiring is already in place; only the texture-content
producer is missing.

### 1.4 Test

Verify gradient brushes render visibly on Rect / RoundedRect / Ellipse / vector-path fills.

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

## Phase 4 — Color improvements [DONE]

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

## Phase 6 — Text layout reuse

**Goal:** Stop allocating a fresh `TextRect`, glyph layout, and GPU
texture on every `drawText` call. Lift the layout/raster product into
a reusable handle that callers hold across frames; `drawText` itself
becomes a one-shot convenience that wraps an ephemeral handle.

**Cross-plan dependencies:**

  - `renderScale` is sourced from `NativeWindow::scaleFactor()` per
    `Native-API-Completion-Proposal.md` §2.2 and flows through the
    visual tree to `View::getRenderScale()` per
    `DPI-Aware-Text-Plan.md`. Phase 6 *consumes* this — it does not
    introduce a new scale source. See §6.3.
  - The platform `TextRect` backends (`DWriteTextRect`, `CTTextRect`,
    `HarfBuzzTextRect`) accept `renderScale` at construction per the
    DPI plan. Phase 6 reuses that contract; the macOS / Linux backend
    work in `DPI-Aware-Text-Plan.md` is a hard prerequisite for Phase
    6 to land cleanly on those platforms (otherwise a held layout
    will rebuild needlessly when scale changes go unnoticed).

### Current state

The text-draw path (the `FrameBuilder` one-shot `drawText`) does the
full pipeline inline on every call:

1. `TextRect::Create(rect, layoutDesc, renderScale)` — allocates a
   platform-specific offscreen surface (DWrite IDWriteTextLayout +
   bitmap render target on Windows; CoreText CTFrame + CGBitmapContext
   on macOS).
2. `GlyphRun::fromUStringAndFont(text, font)` — measures and builds
   the glyph run.
3. `textRect->drawRun(glyphRun, color)` — rasterizes glyphs into the
   offscreen surface.
4. `textRect->toBitmap()` — uploads the surface to a `GETexture` (with
   a fence).
5. emits a `DrawOp::Bitmap` from the uploaded `GETexture`.

For static UI text repainted every frame, steps 1–4 repeat with the
same inputs and the same outputs. On a 60Hz redraw, that's 60
DWrite/CoreText layout calls and 60 GPU uploads per text element per
second, all producing identical bytes. UIView ([UIView.Update.cpp:377](wtk/src/UI/UIView.Update.cpp:377))
hits this path on every paint.

### 6.1 Reusable handle: `TextLayout` (or repurposed `TextRect`)

Either lift `Composition::TextRect` itself out of `FontEngine.h` as
the public handle, or introduce a thin `TextLayout` that wraps a
`TextRect` plus its cached `GETexture`. The latter is less disruptive
because `TextRect` is currently a backend-platform abstract class
(`getNative()`, `drawRun()`, `toBitmap()` are pure virtual on
platform subclasses); wrapping keeps the platform abstraction private.

**Proposed signature:**

```cpp
class OMEGAWTK_EXPORT TextLayout {
    friend class FrameBuilder;
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    static Core::SharedPtr<TextLayout> Create(
        const UniString & text,
        Core::SharedPtr<Font> font,
        const Composition::Rect & rect,
        const Composition::Color & color,
        const TextLayoutDescriptor & layoutDesc);

    /// Mutators — invalidate the cached glyph run and bitmap.
    void setText(const UniString & text);
    void setFont(Core::SharedPtr<Font> font);
    void setRect(const Composition::Rect & rect);
    void setColor(const Composition::Color & color);
    void setLayoutDescriptor(const TextLayoutDescriptor & layoutDesc);

    /// Force a rebuild on the next draw (e.g. after font fallback).
    void invalidate();

    Composition::Rect getRect() const;

    ~TextLayout();
};
```

Note: `Create` does **not** take `renderScale`. The handle is render-
scale-agnostic at construction; the `FrameBuilder` supplies the current
scale on every `drawTextLayout` and the handle rebuilds on mismatch.
See §6.3.

Internally `Impl` owns:

- A lazily-created `TextRect` (via `TextRect::Create(..., renderScale)`,
  built on first resolve with the scale the `FrameBuilder` passes in).
- A lazily-uploaded `TextRect::BitmapRes` (texture + fence).
- The cached `renderScale` the `TextRect` was built against.
- Dirty flags: `layoutDirty_` (text/font/rect/layoutDesc/renderScale
  changed) and `bitmapDirty_` (color changed).

**Resolve:** the first `drawTextLayout` call after a mutator hits the
dirty path:

- `layoutDirty_` → tear down the `TextRect`, build a new one,
  re-rasterize (`drawRun`), re-upload (`toBitmap`).
- `bitmapDirty_` only → keep the `TextRect`, re-rasterize the run
  with the new color, re-upload.
- Neither → emit the cached `Bitmap` command directly.

### 6.2 New draw API (`FrameBuilder`)

```cpp
void drawTextLayout(const Core::SharedPtr<TextLayout> & layout);
```

Implementation: resolve the layout (lazy build), then emit a
`DrawOp::Bitmap` from the cached `GETexture` + the layout's rect. No
allocation, no upload, no DWrite/CoreText round-trip on the steady
path.

The existing `drawText(text, font, rect, color, layoutDesc)` overload
becomes a one-shot convenience:

```cpp
void FrameBuilder::drawText(const UniString & text, ...) {
    auto layout = TextLayout::Create(text, font, rect, color, layoutDesc);
    drawTextLayout(layout);
    // layout falls out of scope after the frame is sent — no caching
}
```

`drawTextLayout` reads `renderScale` from the owner View (§6.3), so
the shim doesn't need to thread it. Existing call sites keep working
unchanged; new code that wants the cache opts in by holding the
handle.

### 6.3 Render scale changes

`renderScale` is a **per-window** quantity, not a `TextLayout`
property. It originates at `NativeWindow::scaleFactor()`
(`Native-API-Completion-Proposal.md` §2.2 — backed by
`GetDpiForWindow` / `NSWindow.backingScaleFactor` / `wl_output`
scale per platform), flows through the visual tree into
`Composition::ViewRenderTarget::renderScale_`, and is read
via `View::getRenderScale()` (`DPI-Aware-Text-Plan.md` §Plumbing).
This is the same plumbing the text-draw path already uses today.

`TextLayout` does **not** read `NativeWindow::scaleFactor()` itself
and does **not** hold a back-pointer to View. The `FrameBuilder` owns
the freshness contract: on every `drawTextLayout`, it reads its owner
View's current `renderScale` and passes it through to the handle's
resolve. The handle compares against the cached scale and treats a
mismatch as a layout-dirty rebuild — same code path as a `setRect` /
`setFont` change. This keeps the handle ignorant of View and lets a
single layout migrate between Views with different scales (rare in
practice but free correctness).

```cpp
// Sketch — FrameBuilder-side:
void FrameBuilder::drawTextLayout(const Core::SharedPtr<TextLayout> & layout) {
    const float scale = (ownerView_ != nullptr)
        ? ownerView_->getRenderScale() : 1.f;
    auto bitmap = layout->resolve(scale); // rebuilds if scale differs
    displayList_.append(DrawOp::Bitmap(bitmap.s, bitmap.textureFence,
                                       layout->getRect()));
}
```

The View-side scale source itself is not Phase 6's concern — it lands
once via §2.2 of the Native API plan (uniform `NativeWindow::scaleFactor()`)
and the platform `TextRect` backends already accept `renderScale` per
the DPI plan. Phase 6 just re-reads the value on each draw instead
of capturing it once at `TextRect::Create` time.

**Out of scope for Phase 6:** the platform-side machinery that
*detects* a per-monitor DPI change and updates `renderScale_`. That
work is owned by `Native-API-Completion-Proposal.md` §2.2 ("DPI scale
change handling") via the `WindowScaleFactorChanged` event, and by
`DPI-Aware-Text-Plan.md` ("Per-monitor DPI updates") for the
`ViewRenderTarget::scaleChanged()` rebuild trigger. Phase 6's per-
draw re-read of `ownerView_->getRenderScale()` means that *once*
those two land, every held `TextLayout` rebuilds its `TextRect` and
re-uploads its `GETexture` on the very next paint, with no
additional code in this phase. The contract is symmetrical: the
event-driven side updates the scale on the View, Phase 6 reads the
View on every draw, and the cache invalidates itself.

### 6.4 UIView migration

`UIView` currently calls `drawText` every paint. Two options:

  - **Opt-in:** UIView keeps a `Core::SharedPtr<TextLayout>` per text
    element on its `UIElementSpec`, lazily creates it on first paint,
    invalidates on text/font/style/rect change. New API surface, but
    the steady-state win is significant.
  - **Implicit cache in the FrameBuilder:** the `FrameBuilder` keeps an
    LRU keyed on (text, font, rect, color, layoutDesc) and reuses behind
    the scenes. Smaller blast radius, but cache invalidation becomes the
    `FrameBuilder`'s problem and key construction is non-trivial
    (UniString hash, Color tolerance, Rect equality across float jitter).

Opt-in is cleaner — invalidation becomes the caller's explicit
responsibility, which matches how UIView already tracks dirty state
on its specs. The implicit cache is a Phase 7 follow-up if profiling
later shows third-party draw callers would benefit.

UIView's `pendingTextHandles_` (or similar field) lives on
`UIView::Impl`. The text-style invalidator already runs on
`onSpecChanged`; extend it to call `layout->invalidate()` /
`setText` / `setColor` on the cached handle for the matching element
tag.

### 6.5 Lifetime / GPU resources

`TextLayout::Impl` owns the `TextRect` and the `GETexture`. As long
as one `SharedPtr<TextLayout>` exists, the GPU texture stays
resident. Frames hold the texture via the `DrawOp::Bitmap`'s
`SharedPtr<GETexture>`, so frame-in-flight safety is already handled
by the existing fence on `BitmapRes::textureFence`.

Two consequences worth flagging:

  - **VRAM growth:** if a UIView retains `TextLayout`s for many
    transient strings (e.g. animated text), they pin GPU textures
    until the View drops them. Phase 7 future: a soft size cap with
    LRU eviction.
  - **Dynamic text:** for text that changes every frame (frame
    counter, timer), `setText()` per frame still pays for a layout
    rebuild + upload. The handle is no worse than today, but the
    benefit is zero. Document this in the API comment.

### 6.6 Test

Visual: render a static label across 600 frames, confirm DWrite /
CoreText is hit only on frame 0 (logging hook in `TextRect::Create`)
and `GETexture` upload count is 1.

Functional: `setText` / `setColor` / `setRect` invalidate correctly;
no stale glyphs persist across rebuilds.

### Files touched

- `wtk/include/omegaWTK/Composition/FontEngine.h` — public
  `TextLayout` declaration (or `TextRect` lifted to public, depending
  on the choice in 6.1).
- `wtk/src/UI/FrameBuilder.{h,cpp}` — `drawText` becomes a thin shim;
  new `drawTextLayout` emits a cached `DrawOp::Bitmap`.
- `wtk/src/Composition/TextLayout.cpp` — new file holding the
  cache/dirty-flag logic.
- `wtk/src/Composition/backend/{mtl,d2d,cairo}/TextRect*.{cpp,mm}` —
  no changes needed if `TextLayout` wraps a backend `TextRect`; minor
  changes if `TextRect` itself moves.
- `wtk/include/omegaWTK/UI/UIView.h` / `wtk/src/UI/UIView.*.cpp` —
  cache `TextLayout` per text-emitting element spec; invalidate on
  spec change.

---

## Phase 8 — Bitmap brushes

**Goal:** Add `Brush::Type::Bitmap` — a fill brush that paints any
shape (Rect / RoundedRect / Ellipse / Path) by sampling a bitmap,
with a wrap mode and a brush-space transform. This promotes the
"Pattern brush" item from Phase 7 Future into a first-class brush.

**Why a brush, not just `DrawOp::Bitmap`.** The compositor already
has a `DrawOp::Bitmap` op that *blits a standalone image into a dest
rect* (used by `UIView`'s image element from Render-Redesign Phase
3.9, and by text bitmap-fallback). A bitmap **brush** is different:
it is a *fill* dispatched through the same `brush->type` switch as
`Color` and `Gradient`, so it composes with shape geometry (rounded
corners, ellipses, arbitrary vector paths, borders) and with the
state stack. The two are complementary — `DrawOp::Bitmap` for "show
this picture here," a bitmap brush for "fill this shape with this
texture."

**Key consequence:** a bitmap brush needs **no new `DrawOp` variant**.
The shape ops (`Rect` / `RoundedRect` / `Ellipse` / `VectorPath`)
already carry a `Core::SharedPtr<Brush>`; a bitmap brush rides that
existing slot and the backend dispatches on `brush->type`. This phase
is therefore entirely in the brush model + backend rasterization +
shader source — squarely on the op-type-agnostic side of the compositor
op model (see *Compositor op model* above).

### 8.1 Brush model

Add to `Brush` (`Brush.h` / `Brush.cpp`):

- `Brush::Type::Bitmap` enum value.
- A `BitmapBrush(...)` factory carrying:
  - the image source — a `Core::SharedPtr<OmegaCommon::Img::BitmapImage>`
    (CPU bitmap, uploaded + cached on first use) or an already-resident
    `OmegaGTE::GETexture` handle (wrapped, per the Phase 0A geometry
    isolation rules — no raw GTE in the public signature);
  - a `WrapMode { Clamp, Tile, Mirror }`;
  - an optional brush-space transform (origin offset + scale) so the
    same texture can tile at different densities.

### 8.2 Backend rasterization

In `RenderTarget.cpp`'s brush dispatch, add the `Brush::Type::Bitmap`
arm. Two routes, mirroring the gradient pipeline split:

- **Texture pipeline (vector paths, the general case).** Upload /
  cache the bitmap to a `GETexture`, bind it with a sampler
  configured for the brush's wrap mode, and sample it from the
  tessellated shape's fragment shader using the interpolated
  shape-local coordinate transformed by the brush transform. Reuses
  the existing tessellation + texture fallback that gradients use.
- **SDF-native (simple primitives).** For Rect / RoundedRect /
  Ellipse, sample the bound texture directly in `sdfFragment` using
  the local coordinate, so the bitmap fill composes with the
  closed-form border + corner radius in one draw call (no separate
  tessellation). This is the same shape of change as the SDF-native
  gradient sampler in Phase 9.3.

Reuse the `Direct-To-Drawable-And-SDF-Plan` §6.6 bitmap improvements
(sampler / mipmap upgrade, source rect, nine-slice) — the bitmap
brush and `DrawOp::Bitmap` share that backend sampling code.

### 8.3 Authoring surface

No `StyleSheet` change is required: `elementBrush(tag, brush)`
already accepts a `SharedHandle<Composition::Brush>`, so a bitmap
brush flows through `UIView`'s element styling unchanged. Later
consumers: SVG `<pattern>` fills and CSS `background-image` map onto
this brush.

### 8.4 Test

A RootWidget / `ImageRenderTest` scene that fills a `RoundedRect`
element with a tiled bitmap brush (wrap = Tile) and a second shape
with wrap = Clamp, validated through the window-scoped `DisplayList`
path. Confirm the fill respects corner radius and border.

### Files touched

- `wtk/include/omegaWTK/Composition/Brush.h` — `Type::Bitmap`,
  `WrapMode`, `BitmapBrush` factory + fields.
- `wtk/src/Composition/Brush.cpp` — factory implementation.
- `wtk/src/Composition/backend/RenderTarget.cpp` — `Bitmap` brush
  dispatch (texture upload/cache, sampler, SDF + tessellation routes).
- `wtk/src/Composition/backend/shaders/compositor.omegasl` — texture
  sampling in `sdfFragment` for the bitmap-brush case; wrap-mode
  sampler.

---

## Phase 9 — Finish gradients

**Goal:** Close out the gradient pipeline so `GradientBrush(...)`
renders correctly on every primitive. This is the consolidated
"definition of done" for gradients — it sequences the remaining
Phase 1 (texture path) and Phase 2 (geometry) work plus the
SDF-native sampler (was a Phase 7 Future item) into one shippable
closeout, framed for the `DrawOp` / SDF model.

Like Phase 8, this is **op-type-agnostic** — it lives in the brush
model, `RenderTarget.cpp`, and the shader source, all of which read
brush payloads, not the op wrapper.

### 9.1 Land the texture path (closes Phase 1)

Complete Phase 1.1–1.4 as written: implement the `linearGradient` /
`radialGradient` compute shaders, finish the `createGradientTexture`
const-buffer write, and populate the texture binding on the
gradient-brush fallback so a gradient fill produces a visible
texture on Rect / RoundedRect / Ellipse / vector-path fills.

### 9.2 Land the geometry (closes Phase 2)

Replace the single `float arg` with `LinearDef` (start/end points),
`RadialDef` (center / focus / elliptical radii), and a
`GradientSpread { Pad, Repeat, Reflect }`. The texture producer in
9.1 reads these instead of just an angle.

### 9.3 SDF-native gradient sampler (simple primitives)

For Rect / RoundedRect / Ellipse, extend `sdfFragment` to evaluate
the gradient parameter `t` analytically from the interpolated local
coordinate (using the 9.2 geometry), look up stop colors from a
small constant-array uniform, and skip the compute pass + texture
allocation entirely. Vector-path gradient fills keep the 9.1 texture
path (their fragment shader is `mainFragment`, color-attachment
driven). This is the common-case win and shares its shape with the
Phase 8.2 SDF-native bitmap sampler.

### 9.4 SVG gradient import

Wire SVG `<linearGradient>` / `<radialGradient>` (with `gradientUnits`,
stops, and spread) onto the finished gradient API in `SVGView`,
replacing today's opacity-into-`Color.a` workaround for gradient
fills.

### 9.5 Test

Gradient fills on Rect / RoundedRect / Ellipse / vector path, both
spread modes (Repeat / Reflect), linear and radial, validated through
the window-scoped `DisplayList` path. A RootWidget scene plus an SVG
document with native gradients (`SVGViewRenderTest`).

### Files touched

- `wtk/src/Composition/backend/RenderTarget.cpp` — gradient compute
  pass wiring, extended gradient params, texture binding.
- `wtk/src/Composition/backend/RenderTarget.h` — `createGradientTexture`
  signature.
- `wtk/src/Composition/backend/shaders/compositor.omegasl` —
  `linearGradient` / `radialGradient` compute functions; SDF-native
  gradient sampler in `sdfFragment`; extended `GradientTextureConstParams`.
- `wtk/include/omegaWTK/Composition/Brush.h` / `wtk/src/Composition/Brush.cpp`
  — `LinearDef` / `RadialDef` / `GradientSpread`, new gradient factories.
- `wtk/src/UI/SVGView.cpp` — native gradient import.

---

## Phase 10 — MSDF scalable bitmaps (supersedes nine-slice for shape assets)

**Goal:** Retire nine-slice (`BitmapParams::NineSliceInsets`,
`Direct-To-Drawable-And-SDF-Plan.md` §6.6.3) as the *primary* resize
strategy for **shape-class** UI assets by storing them as MSDF tiles and
reconstructing them at draw time with the same median → `smoothstep`
coverage path that §6.7 already ships for text. Nine-slice and the
textured-quad path (§6.6.1 / §6.6.2) stay as the fallback for raster
content that isn't a distance-field-expressible shape.

Like Phases 8 and 9 this is **op-type-agnostic** — it lives in the
asset / brush model, `RenderTarget.cpp`, and the shader source, all
reading payloads, so it is independent of the compositor op type.

**Scope boundary (deliberate).** MSDF encodes distance to a shape edge
and reconstructs as a resolution-independent *coverage mask* that you
tint — a 2-region (inside / outside) + color model. That is strictly
better than nine-slice for the *shape / chrome* class of resizable
bitmaps (rounded-rect buttons, panels, speech bubbles, icons,
single-silhouette art), which is the dominant use of nine-slice. It
**cannot** represent continuous-tone, multi-color raster content (a
photo, a gradient-baked texture, a logo with interior detail) — there is
no single edge to take a distance to. This phase therefore targets shape
assets only; arbitrary raster content stays on §6.6, and nine-slice
survives as the explicit fallback for raster chrome that can't be
vectorized.

### 10.0 Why this supersedes nine-slice

Nine-slice exists because a raster button background distorts its corners
when stretched. It pays for that with: authored slice guides per asset, a
full raster at one resolution (blurry when upscaled past native), and 9
sub-quads per draw. An MSDF of the same shape:

- scales to **any** destination rect with crisp corners and edges — no
  insets to author, no native-resolution ceiling;
- is a small tile (a 64–128px MSDF covers a button rendered at any
  size), not a full-res raster;
- composes for free with tint, outline, soft shadow, and glow (the
  `dist > 0` band, `Direct-To-Drawable-And-SDF-Plan.md` §6.7.3);
- because it rides the SDF primitive path, it composes with a shape's
  **corner radius and border** in one draw (same shape of change as the
  Phase 8.2 / 9.3 SDF-native samplers).

It is **not** a replacement for sampling arbitrary color pixels.

### 10.1 Generalize the MSDF tile producer

§6.7's `GlyphAtlas` (`wtk/src/Composition/backend/GlyphAtlas.{h,cpp}`) is
a per-font MSDF cache: a `RasterizeFn(glyphId) → MSDF tile` callback, a
shelf packer, and an `AtlasGlyph` (`tileOriginX/Y`, `tileScale`, UV
rect). The MSDF machinery in it is not glyph-specific — only the
`RasterizeFn` is. Lift the cache into a reusable `MSDFAtlas` keyed on an
opaque asset id, with two `RasterizeFn` flavors:

- **Vector source (the clean win).** The asset is already a contour set
  — an SVG `<path>` / `GVectorPath2D`, a `Path`, or a built-in shape.
  Feed contours straight into msdfgen (`edgeColoringSimple(shape, 3.0)`
  → `generateMSDF` into `Bitmap<float,3>`), the exact path §6.7-c2 runs
  for glyph outlines. Sharp corners preserved, no raster ever involved.
- **Raster mask source.** The asset is a bitmap silhouette (e.g. a
  1-bit / alpha icon). Threshold the alpha, trace contours
  (marching-squares), then run the same msdfgen path. Lower fidelity
  than a true vector source — authored vector input is preferred.

`AtlasGlyph` generalizes to `AtlasTile` (UV rect + `tileOrigin` /
`tileScale`); `ensureGlyph` becomes `ensureTile(assetId)`. The glyph
atlas becomes a typed instantiation of the generalized atlas so §6.7 is
unchanged behaviorally.

### 10.2 Asset / API surface

Two integration points. Land the standalone-op path first — that is where
nine-slice lives today, so it is the direct supersession:

- **Standalone image op (direct nine-slice replacement).** Add an MSDF
  flavor to the standalone bitmap path that `NineSliceInsets` rides
  today (`BitmapParams` / `DrawOp::Bitmap`). A `ScalableImage` source
  carries an `MSDFAtlas` tile handle + a base tint; drawing a
  `ScalableImage` into any dest rect samples the tile — no insets, no
  slice math. The existing nine-slice image path stays for raster
  assets.
- **Bitmap brush (Phase 8 extension).** A `Brush::Type::Bitmap` whose
  source is a `ScalableImage` dispatches to the MSDF arm instead of the
  texture-sample arm, so a rounded-rect / ellipse / path **fill** gets a
  resolution-independent shape mask that composes with corner radius and
  border. Natural once Phase 8 lands; shares 10.3's shader arm.

### 10.3 Shader

Reuse `msdfTextFragment`'s reconstruction verbatim —
`median(s.r,s.g,s.b)` → `aa = fwidth(median)` →
`smoothstep(0.5±aa, median)` → `tint × coverage × currentOpacity`. Add it
as an arm in the relevant fragment paths rather than a new shader:

- In `sdfFragment` (`compositor.omegasl`), when the primitive carries an
  MSDF-image payload, sample the bound MSDF tile at the interpolated
  local coord, take coverage, and **intersect** it with the existing
  shape coverage (corner radius / border distance). This is the one-draw
  compose-with-shape route.
- For the standalone-op path, an MSDF arm in `bitmapFragment` sampling
  the tile and emitting `tint × coverage`.

Sampler: linear filtering on the MSDF tile (the §6.6.1 sampler upgrade
already gives this); the distance field interpolates correctly under
magnification, which is the whole point — no mipmaps needed for the field
itself.

### 10.4 Migration

1. Land 10.1 (generalized atlas) with the glyph atlas re-expressed on top
   of it — no behavior change to text.
2. Land the vector-source `RasterizeFn` and the standalone `ScalableImage`
   op + 10.3 `bitmapFragment` arm.
3. Convert the engine's own shape-class chrome assets (and SVG-sourced
   icons, which are *already vector* — they skip raster entirely) from
   nine-slice to `ScalableImage`.
4. Mark `NineSliceInsets` / the nine-slice image path
   **legacy / fallback** in the API docs: "for raster chrome that can't
   be expressed as a distance field; prefer `ScalableImage` for shape
   assets." Do not delete it — it's the honest fallback for raster
   content.
5. (Follow-up) Raster-mask `RasterizeFn` + contour tracing for icon
   bitmaps with no vector original.

### 10.5 Test

A RootWidget scene rendering one shape-class asset (a rounded button
background) at three wildly different sizes from a single MSDF tile —
confirm crisp corners at all scales with one upload, vs. the nine-slice
version's native-resolution blur on upscale. A second case fills a
`RoundedRect` element with a `ScalableImage` bitmap brush (Phase 8) and
confirms the MSDF coverage intersects the corner radius + border in one
draw. Validated through the window-scoped `DisplayList` path.

### Files touched

- `wtk/src/Composition/backend/GlyphAtlas.{h,cpp}` — generalize to
  `MSDFAtlas` / `AtlasTile`; glyph atlas becomes a typed instantiation.
- `wtk/include/omegaWTK/Composition/Brush.h` / `wtk/src/Composition/Brush.cpp`
  — `ScalableImage` source; `Brush::Type::Bitmap` MSDF dispatch (Phase 8
  extension).
- `wtk/src/UI/FrameBuilder.{h,cpp}` — `ScalableImage` standalone-image
  entry; `NineSliceInsets` docs marked legacy / fallback.
- `wtk/src/Composition/backend/RenderTarget.cpp` — MSDF-image dispatch arm
  (tile bind + payload).
- `wtk/src/Composition/backend/shaders/compositor.omegasl` — MSDF arm in
  `sdfFragment` (compose-with-shape) and `bitmapFragment` (standalone),
  reusing the `msdfTextFragment` reconstruction.

---

## Phase 11 — Glass brushes

**Goal:** Add `Brush::Type::Glass` — a fill brush that paints any
shape (Rect / RoundedRect / Ellipse / Path) as a translucent **glass
material**: a blurred, refracted sample of the backdrop *behind* the
shape, tinted (solid → `ColoredGlass`, or gradient → `GradientGlass`),
with a fresnel rim highlight and a specular streak. This is WTK's
analogue of Apple's "Liquid Glass" material.

**Scope decisions (locked at proposal time):**

  - **Closed brush type, not an open shader-brush system.** Glass is a
    fixed `Brush::Type::Glass` with a `GlassMaterial` payload. It is
    *implemented* with a dedicated compositor shader, but there is **no
    third-party shader registration, no runtime param reflection, no
    user-authored `.omegaslh` snippets**. If a general `ShaderBrush`
    mechanism is wanted later, this phase is its first concrete
    instance and proves out the seams (shared SDF header, dedicated
    pipeline, brush-payload marshalling) — but that generalization is
    explicitly out of scope here.
  - **Refraction-first for the high tier.** The default high-quality
    `GlassBrush` (the `Full` tier, §11.2) captures and blurs the
    backdrop and refracts it — it looks like real glass, not a tinted
    translucent panel. Backdrop capture (§11.4) is the visual point of
    the feature, so it lands in the first increment rather than being
    deferred behind a cheaper approximation.
  - **A cheap tier as a safety for weak/older hardware.** Per-shape
    backdrop capture + blur + refraction is too expensive for genuinely
    low-horsepower GPUs (older / entry-level parts). The `Cheap` tier
    (§11.2) shares **one** window-scoped downsampled backdrop across all
    glass shapes and skips per-shape refraction. It is an explicit
    app/user setting (plus an optional adaptive frame-budget drop) —
    **not** keyed on the integrated/discrete bit, since modern unified-
    memory GPUs (Apple Silicon M-series Max/Ultra, current laptop parts)
    run `Full` comfortably. This also subsumes the degenerate
    "allocation failed" fallback.
  - **Clear vs. Frosted finish.** Glass has two finishes (§11.1):
    *Clear* (light blur, strong refraction — see shapes through it,
    distorted) and *Frosted* (heavy blur, ~no refraction — diffuse,
    colors-not-shapes). Frosted is the cheap-friendly finish: it reads
    almost identically at the `Cheap` tier because diffuse blur hides
    the shared-backdrop imprecision, whereas `Clear` loses its sharp
    refraction when downgraded.

**Why a brush, not a layer effect.** A glass fill is dispatched through
the same `brush->type` switch as `Color` and `Gradient`
([RenderTarget.cpp](../src/Composition/backend/RenderTarget.cpp) brush
dispatch), so it composes with shape geometry (corner radius, ellipse,
arbitrary vector path, border) and with the state stack in **one draw**.
A `LayerBlur` ([Layer.h](../include/omegaWTK/Composition/Layer.h)) blurs
a layer's *own* content; a glass brush blurs the *backdrop beneath an
arbitrary shape* and tints/refracts it. They are complementary and
share the scratch + compute-blur machinery (§11.4).

**Op-type-agnostic.** Like Phases 8–10, glass lives in the brush model,
`RenderTarget.cpp`, and the shader source — all reading payloads — so it
is independent of the compositor op type. **No new `DrawOp`
variant**: a glass brush rides the existing `Core::SharedPtr<Brush>`
slot on the shape ops.

### 11.1 Brush model

Add to `Brush` (`Brush.h` / `Brush.cpp`):

- `Brush::Type::Glass` enum value.
- A `GlassMaterial` payload:

```cpp
struct GlassMaterial {
    enum class Tint    : uint8_t { Solid, Gradient } tintKind = Tint::Solid;
    enum class Finish  : uint8_t { Clear, Frosted };   // §11.1 finish presets
    enum class Quality : uint8_t { Auto, Full, Cheap }; // §11.2 perf tier

    Finish   finish  = Finish::Clear;
    Quality  quality = Quality::Auto;   // Auto resolves per-device (§11.2)

    Color    color;        // ColoredGlass tint (tintKind == Solid)
    Gradient gradient;     // GradientGlass tint (tintKind == Gradient)

    // Raw knobs. Left at sentinel (< 0) they inherit from `finish`:
    //   Clear   → backdropBlur ≈ 4,  refraction ≈ 0.06
    //   Frosted → backdropBlur ≈ 20, refraction ≈ 0.0
    float backdropBlur  = -1.f;   // frosted-ness; drives the §11.4 blur
    float refraction    = -1.f;   // edge-bend strength of the backdrop sample
    float edgeHighlight = 0.5f;   // fresnel rim sheen intensity [0,1]
    float specular      = 0.4f;   // specular streak intensity   [0,1]
    float lightAngle    = 0.785f; // streak direction (radians)
    float tintOpacity   = 0.85f;  // how much tint vs. raw backdrop shows
};
```

`finish` is a preset over `backdropBlur` + `refraction`; an explicit
non-negative value on either knob overrides the preset. `finish` is
orthogonal to both the tint source (solid / gradient) and the quality
tier — you can have frosted gradient glass on the cheap tier.

- Factories parallel to the existing `ColorBrush` / `GradientBrush`
  free functions ([Brush.h:141](../include/omegaWTK/Composition/Brush.h:141)).
  The `Finish` defaults to `Clear`; `FrostedGlassBrush` is the named
  sugar for `Finish::Frosted` (discoverability — frosted is a first-
  class option, not a flag a caller has to know to set):

```cpp
// Clear by default; pass Finish::Frosted for frosted.
OMEGAWTK_EXPORT Core::SharedPtr<Brush> ColoredGlassBrush(const Color & tint,
        GlassMaterial::Finish finish = GlassMaterial::Finish::Clear);
OMEGAWTK_EXPORT Core::SharedPtr<Brush> GradientGlassBrush(const Gradient & tint,
        GlassMaterial::Finish finish = GlassMaterial::Finish::Clear);

// Named frosted convenience (solid + gradient overloads):
OMEGAWTK_EXPORT Core::SharedPtr<Brush> FrostedGlassBrush(const Color & tint);
OMEGAWTK_EXPORT Core::SharedPtr<Brush> FrostedGlassBrush(const Gradient & tint);

// Full control (finish, quality, every knob):
OMEGAWTK_EXPORT Core::SharedPtr<Brush> GlassBrush(const GlassMaterial & material);
```

**Union-destructor gotcha (must get right first).** `Brush` holds a
`union { Color color; Gradient gradient; }` with a hand-written
`~Brush()` that type-dispatches the placement-destruct. `GlassMaterial`
embeds a `Gradient` (which owns an `OmegaCommon::Vector<GradientStop>`),
so adding it to the union grows that destructor a `Glass` arm and the
constructors a placement-`new` arm. Skipping this is a leak / UB, not a
cosmetic miss. Mirror the existing `Gradient` arm exactly.

### 11.2 Quality tiers & cheap fallback

Per-shape backdrop capture + blur + refraction (§11.4) is the right
look but the wrong cost on weak/older GPUs — it is a capture blit plus
two compute passes plus a textured draw *per glass shape, per frame*.
`GlassMaterial::Quality` selects the tier:

  - **`Full`** — per-shape backdrop capture + blur + refraction (§11.4).
    Highest fidelity, highest cost. The default on any modern GPU,
    **integrated or discrete**.
  - **`Cheap`** — capture **one** window-scoped, downsampled backdrop
    **once per frame** (not per shape) and blur it once; every glass
    shape samples that shared texture at its own bounds with tint + rim
    + specular and *no* per-shape refraction. Cost is one capture + one
    blur per frame regardless of how many glass shapes are on screen.
    This also subsumes the §11.4 degenerate fallback.
  - **`Auto`** (default) — `Full`. Modern GPUs, **integrated or
    discrete**, run per-shape glass fine: unified-memory Apple Silicon
    (M-series Max/Ultra) and current laptop GPUs are not "low-power" in
    any sense that warrants downgrading, so `Auto` deliberately does
    **not** key off `GTEDevice::type` — the integrated/discrete bit is
    not a horsepower proxy. The drop to `Cheap` is a *safety for
    genuinely weak/older hardware*, driven by:
      - an explicit app/user low-power setting (compositor-level), and
      - optionally an adaptive frame-budget watchdog — if glass frames
        consistently overrun the target, drop the tier; a *measured*
        signal beats guessing from the device descriptor.

    A per-brush `quality` other than `Auto` overrides both. (`GTEDevice`
    capability flags / `queryMemoryBudget()` —
    [GTEDevice.h:151](../../gte/include/omegaGTE/GTEDevice.h:151) — can
    still gate truly ancient parts, but are not the primary trigger.)

**Why Cheap still looks like glass.** A single shared backdrop loses
per-shape parallax and sharp refraction but keeps backdrop *color and
luminance* under each shape — far better than a flat tint. **Frosted**
finishes (§11.1) are near-indistinguishable from `Full` at this tier
because their heavy diffuse blur hides the shared-backdrop imprecision;
**Clear** finishes visibly lose their refraction, so a `Clear` material
auto-downgraded to `Cheap` is the one case worth a design look.

### 11.3 Shared shader header — `compositor_sdf.omegaslh`

`.omegaslh` headers hold **declarations only** (no entry points — the
OmegaSL preprocessor rejects a `fragment`/`vertex`/`compute` in a
header; see `gte/omegasl/tests/include_shader_in_header.omegaslh`).
Factor the closed-form distance helpers out of `compositor.omegasl`
([sdfRect / sdfRoundedRect / sdfEllipse, lines 370–404](../src/Composition/backend/shaders/compositor.omegasl:370))
into a new `compositor_sdf.omegaslh`, and add the glass helpers there:

```
// compositor_sdf.omegaslh — shared, no entry points
float sdfRect(float2 p, float2 b);
float sdfRoundedRect(float2 p, float2 b, float r);
float sdfEllipse(float2 p, float2 r);

float  fresnelEdge(float dist, float edgeWidth);          // rim sheen ramp
float2 refractOffset(float2 local, float grad, float k);  // backdrop UV bend
float  specularStreak(float2 local, float angle);         // directional highlight
```

`compositor.omegasl` `#include`s it (the existing `sdfFragment` keeps
working unchanged — it now calls the helpers from the header), and the
glass fragment (§11.5) includes the same header so it reuses the exact
distance vocabulary. This is the "new path for shader brushes" seam:
one shared header, distinct entry points per pipeline.

### 11.4 Backdrop capture + blur (Full tier — the frame-graph piece)

This is the `Full` tier (§11.2); the `Cheap` tier captures one shared
backdrop per frame instead of doing this per shape. Glass is a
**backdrop** effect, so the fragment must read pixels already
composited *under* the shape. In the Direct-To-Drawable immediate pass,
reading the drawable being written is undefined — so the backdrop is
**snapshotted into a texture before the glass draw**. This reuses two
existing mechanisms:

  - the **`LayerBlurScratch` ping-pong + compute blur** path
    (`renderBlurredSlice`, [RenderTarget.cpp:1675](../src/Composition/backend/RenderTarget.cpp:1675);
    `gaussianBlurH/V` at [compositor.omegasl:211](../src/Composition/backend/shaders/compositor.omegasl:211)),
    inverted: capture the **destination region under the shape's
    bounds** instead of the layer's own content;
  - the **Phase G content-capture marker** (capture at the shape's
    layout rect), which already does drawable-subregion capture.

Flow at a glass op:

1. Flush prior ops in the shape's bound region to the drawable (glass
   must refract everything beneath it in z-order — in immediate mode,
   everything submitted so far this frame in that region).
2. Blit the drawable subregion under the shape bounds (padded by the
   refraction reach) into a `backdrop` scratch texture.
3. Run `gaussianBlurH/V` on the scratch at `material.backdropBlur`
   (skip when `backdropBlur <= 0`).
4. Bind the blurred scratch as `backdropTex` for the glass draw.

**Key risk — this is the only frame-graph change in the phase.** The
ordering constraint (capture *after* prior siblings, *before* the glass
shape) and read-after-write hazards on the drawable are the real work;
the blur and composite are already-proven code. A degenerate fallback
(allocation fails / no active frame) drops to the §11.2 `Cheap` path —
tint + rim + specular over the shared backdrop, or straight alpha if
even that is unavailable — so a shape never disappears, exactly as
`renderBlurredSlice` falls back to the unblurred direct path today.

### 11.5 Glass pipeline + `glassFragment`

A **dedicated glass pipeline** (its own `glassParams` uniform buffer +
`backdropTex` binding), not extra lanes on `OmegaWTKSdfDrawParams` —
keeps the hot color-SDF path lean and gives the payload room for the
full material. Reuse the existing `sdfVertex` local-coord varying
([compositor.omegasl:362](../src/Composition/backend/shaders/compositor.omegasl:362)).

`glassFragment` (new entry point in `compositor.omegasl`, including
`compositor_sdf.omegaslh`):

1. Evaluate the shape SDF for the primitive `kind` — **composes with
   corner radius + border in one draw**, same as `sdfFragment`.
2. Sample `backdropTex`: on `Full`, offset the UV by `refractOffset`
   from the local gradient of `dist` (per-shape refraction); on `Cheap`,
   sample the shared backdrop straight. A `quality` flag in `glassParams`
   selects the branch.
3. Tint: `Solid` → `material.color`; `Gradient` → the **SDF-native
   analytic gradient evaluator from Phase 9.3** (so `GradientGlass` is
   nearly free once 9.3 lands), modulated by the backdrop sample at
   `tintOpacity`.
4. Add `fresnelEdge` rim sheen and `specularStreak` along `lightAngle`.
5. Mask the whole thing by the SDF fill coverage (and stroke band for a
   bordered glass shape); multiply by `currentOpacity`.

### 11.6 Backend dispatch

Add the `Brush::Type::Glass` arm to the brush switch in
[RenderTarget.cpp:1941](../src/Composition/backend/RenderTarget.cpp:1941),
beside the `Color` (SDF-native) and gradient (texture-fallback) arms.
Resolve the tier first (`material.quality`, or the §11.2 `Auto`
policy), then:

  - **`Full`** — **Rect / RoundedRect / Ellipse** run the §11.4 per-shape
    capture, then the §11.5 glass pipeline (SDF-native; composes with
    border + corner radius).
  - **`Cheap`** — bind the once-per-frame shared backdrop (captured at
    frame start when any glass shape is present) and run the same §11.5
    pipeline with the refraction branch off.
  - **VectorPath** — the tessellation + texture fallback that gradient
    and bitmap brushes use; the glass fragment samples the backdrop with
    the path's interpolated local coord. Lower priority — glass panels
    are overwhelmingly rounded rects / capsules.

### 11.7 Authoring surface

No `StyleSheet` change: `elementBrush(tag, brush)` already accepts a
`SharedHandle<Composition::Brush>` (same as Phase 8), so a glass brush
flows through `UIView` element styling unchanged. A frosted capsule is
then `elementBrush(tag, FrostedGlassBrush(Color::White.withAlpha(0.3f)))`;
a clear tinted panel is `elementBrush(tag, ColoredGlassBrush(tint))`.

### 11.8 Test

A RootWidget scene with a colorful gradient/image backdrop and several
glass shapes over it: a `RoundedRect` with clear `ColoredGlass` + a
border (confirm the backdrop refracts and the fill respects corner
radius + border in one draw), an `Ellipse` with clear `GradientGlass`
(confirm the gradient tint modulates the backdrop and the rim sheen
tracks `lightAngle`), and a `FrostedGlassBrush` panel (confirm the
diffuse frosted blur). Render each at `Quality::Full` and
`Quality::Cheap` and confirm the cheap tier still shows backdrop color
from one shared capture (frosted near-identical; clear visibly less
refractive). Validated through the window-scoped `DisplayList` path.
**Visual verification is mandatory** (AGENTS.md §Visual Debugging) — a
glass material that passes a pixel-diff but reads as flat acrylic has
missed the point; hand a screenshot to the user.

### Files touched

- `wtk/include/omegaWTK/Composition/Brush.h` — `Type::Glass`,
  `GlassMaterial` (incl. `Finish` / `Quality`), `ColoredGlassBrush` /
  `GradientGlassBrush` / `FrostedGlassBrush` / `GlassBrush` factories.
- `wtk/src/Composition/Brush.cpp` — factory impls; finish→knob preset
  resolution; **union ctor/dtor `Glass` arm** (§11.1 gotcha).
- `wtk/src/Composition/backend/shaders/compositor_sdf.omegaslh` —
  **new** shared header: SDF distance helpers (moved out of
  `compositor.omegasl`) + `fresnelEdge` / `refractOffset` /
  `specularStreak`.
- `wtk/src/Composition/backend/shaders/compositor.omegasl` — `#include`
  the new header; **new** `glassFragment` + `glassParams` /
  `backdropTex` bindings; `sdfFragment` switched to the header helpers.
- `wtk/src/Composition/backend/RenderTarget.cpp` — `Type::Glass` brush
  dispatch; `Quality::Auto` = Full, with an explicit low-power setting
  (and optional adaptive frame-budget drop) selecting Cheap — not keyed
  on the integrated/discrete bit; Full-tier per-shape capture (invert
  `LayerBlurScratch`) + Cheap-tier shared once-per-frame backdrop; glass
  pipeline state object + param marshalling.
- `wtk/src/Composition/backend/RenderTarget.h` — glass pipeline / param
  declarations.
- A new `wtk/tests/.../GlassBrushTest` scene (per the
  `gte/Tests/assets/<TestName>/` test-asset convention).

**Cross-backend note.** The shader is authored once in OmegaSL and
compiled to MSL / HLSL / GLSL by `omegaslc`; per-backend divergences
only surface in the real compilers, so syntax-check the `-S` HLSL/GLSL
output with local `dxc` + `glslc`, and hand Windows/Linux runtime builds
to the user (WSL constraint). Only the Metal runtime compiles on the
macOS host.

### Dependencies / sequencing

  - **Soft dep on Phase 9.3** (SDF-native gradient evaluator) for
    `GradientGlass` — `ColoredGlass` ships without it; `GradientGlass`
    reuses 9.3's analytic stop evaluation rather than a second copy.
  - Reuses the **Phase G** content-capture marker and the existing
    **`LayerBlur`** scratch + `gaussianBlurH/V` compute (§11.4) — no new
    blur kernel.
  - `Quality::Auto` defaults to `Full`; the `Cheap` safety drop is an
    explicit setting or an optional frame-budget watchdog — it does
    **not** key off `GTEDevice::type` (integrated ≠ low-power).
  - Op-type-agnostic — independent of the compositor op wrapper.

---

## Phase 12 — Bitmap tinting (Color & Gradient)

**Goal:** Tint a raster bitmap at draw time by a solid **Color** or a
**Gradient**, under one of two blend semantics — **Multiply** (modulate:
`result = texel × tint`) or **Mask** (recolor: `rgb = tint.rgb`,
`coverage = texel.a × tint.a`). Applies to both the standalone bitmap
draw (`DrawOp::Bitmap`, the `Image`/`ImageIcon` path) and the Phase 8
bitmap **brush** (a shape *filled* with a tinted bitmap).

Like Phases 8–11 this is **op-type-agnostic** — it lives in the bitmap
payload, `RenderTarget.cpp`'s bitmap dispatch, and the bitmap fragment
shader, all reading payloads, so it is independent of the compositor op
wrapper. **No new `DrawOp` variant** — the tint rides the existing
`DrawOp::Bitmap` payload and (Phase 8) the `Brush` payload.

### Current state — solid-Color *multiply* already ships

This phase is an **extension of an existing path**, not greenfield.
Solid-color multiply tint on the standalone bitmap op is already wired
end-to-end:

- `DrawOp::Bitmap` carries `Core::Optional<Composition::Color> tintColor`
  ([DisplayList.h:211](../include/omegaWTK/Composition/DisplayList.h:211))
  with a constructor overload that accepts it
  ([DisplayList.h:330](../include/omegaWTK/Composition/DisplayList.h:330)).
- The backend resolves it to an RGBA tint and forwards it to
  `emitBitmapPrimitive`
  ([RenderTarget.cpp:2046](../src/Composition/backend/RenderTarget.cpp:2046)).
- `bitmapFragment` multiplies component-wise, `result = c * tint`, with
  identity `(1,1,1,1)` collapsing to passthrough × opacity
  ([compositor.omegasl:115](../src/Composition/backend/shaders/compositor.omegasl:115)).

So **Color + Multiply is done.** What's missing, and what this phase
adds: the **Mask (recolor)** blend semantic, the **Gradient** tint
source, and carrying both through the Phase 8 bitmap **brush**.

### 12.1 Tint model — `BitmapTint`

Replace the bare `Core::Optional<Composition::Color> tintColor` on the
bitmap payload with a richer descriptor. It lives in `Brush.h` (next to
`Gradient`, which it references) so both the `DrawOp::Bitmap` payload and
the Phase 8 `BitmapBrush` share one type:

```cpp
struct OMEGAWTK_EXPORT BitmapTint {
    enum class Source : uint8_t { Color, Gradient };
    enum class Mode   : uint8_t {
        Multiply,   // result = texel × tint          (dim / modulate — today's path)
        Mask        // rgb = tint.rgb; a = texel.a × tint.a   (recolor)
    };

    Source source = Source::Color;
    Mode   mode   = Mode::Multiply;
    Color    color {};       // used when source == Color
    Gradient gradient {};    // used when source == Gradient (shape-local coords)

    // Back-compat: an existing call site passing a Color still means
    // "solid multiply". Keeps the ~2 in-tree tintColor sites compiling
    // through the migration.
    BitmapTint() = default;
    BitmapTint(const Color & c) : source(Source::Color), mode(Mode::Multiply), color(c) {}

    static BitmapTint ColorTint(const Color & c, Mode m = Mode::Multiply);
    static BitmapTint GradientTint(const Gradient & g, Mode m = Mode::Multiply);
};
```

The bitmap payload field becomes `Core::Optional<BitmapTint> tint`
(absent ⇒ identity passthrough). The existing `Color`-typed constructor
overloads on `DrawOp::Bitmap` keep working via the implicit
`BitmapTint(const Color&)`.

**Gradient coordinate space.** The gradient is evaluated in the bitmap's
**dest-rect-local** space (`[0,0]`..`[1,1]` across the drawn quad), the
same shape-relative convention as Phase 2.1 / 9.2 gradient geometry — so
a vertically-tinted icon is `GradientTint(Gradient::Linear({...}, 0,0, 0,1))`.

### 12.2 Mask (recolor) semantic — standalone op

Add the `Mode::Mask` arm to `bitmapFragment`. The fragment already has
the sampled texel `c` and the resolved `tint`; branch on a `tintMode`
uniform:

```
// c = sample(...); tint = resolved rgba (color or gradient — see 12.3)
if (tintMode == 0 /*Multiply*/) {
    result = c * tint;                       // existing path
} else /*Mask*/ {
    result.rgb = tint.rgb;                   // recolor
    result.a   = c.a * tint.a;               // source alpha is the coverage mask
}
```

Extend `OmegaWTKBitmapDrawParams` (currently just `float4 tintColor`,
[compositor.omegasl:97](../src/Composition/backend/shaders/compositor.omegasl:97))
with `uint tintMode` and `uint tintSource`. `emitBitmapPrimitive`
([RenderTarget.cpp:1044](../src/Composition/backend/RenderTarget.cpp:1044))
writes them from the resolved `BitmapTint`.

*Why Mask matters:* multiplying an arbitrarily-colored icon by a tint
darkens it, it does not recolor it (only a *white* source recolors under
multiply). Mask makes recolor work for any source — this is the semantic
the deferred `ImageIcon` tint (Widget-Stub-Implementation-Plan Phase 2B)
needs.

### 12.3 Gradient tint source — standalone op (soft dep on Phase 9.3)

When `source == Gradient`, the per-fragment tint color is evaluated from
the gradient instead of read from a constant. This **reuses the Phase 9.3
SDF-native analytic gradient evaluator** rather than the compute-shader
texture path — the same way `GradientGlass` (Phase 11.5 step 3) reuses
it. `ColorTint` ships without 9.3; `GradientTint` is gated on it.

- **Shader:** give the bitmap fragment a `local` varying (0..1 across the
  dest quad), distinct from the texture `uv` (which differs under
  sub-rect sampling). The CPU authors it at quad-build time (corners
  `0,0 / 1,0 / 0,1 / 1,1`). Extend `OmegaWTKBitmapDrawParams` with the
  gradient stop array + geometry (`LinearDef`/`RadialDef`) + spread,
  reusing the 9.3 uniform layout, and call the shared gradient-eval
  helper (from `compositor_sdf.omegaslh`, introduced in 9.3 / 11.3) to
  produce `tint` at `local`. Mask/Multiply then apply exactly as 12.2.
- **Backend:** `emitBitmapPrimitive` marshals the gradient stops/geometry
  into `bitmapParams` when `source == Gradient` — same marshalling the
  9.3 SDF-native path performs, factored into a shared helper.

If Phase 9.3 has not landed, `GradientTint` can fall back to the Phase 1
gradient **texture** (rasterize the gradient into a texture, sample it in
`bitmapFragment` and use it as the tint) — but the SDF-native evaluator
is preferred and is the target here.

### 12.4 Bitmap-brush tint (Phase 8 extension)

Once Phase 8 (`Brush::Type::Bitmap`) lands, a shape *filled* with a
bitmap gets the same `BitmapTint` field on the brush. The brush's
fill fragment (SDF-native for simple primitives; tessellation + texture
for vector paths, per Phase 8.2) applies the identical Multiply/Mask +
Color/Gradient logic, with the gradient evaluated in the shape's local
space. This is purely additive to Phase 8's dispatch arm — no new plumbing
beyond threading `BitmapTint` into the brush payload and the fill shader.

*Relation to Phase 10.* MSDF `ScalableImage` already carries a solid
"base tint" (§10.2) applied as `tint × coverage`; a **gradient** base
tint there is a natural follow-up that reuses this phase's gradient-eval
call (coverage × evaluated-gradient) — noted, not in scope for Phase 12.

### 12.5 Consumers

- **`Image` widget** — a `Multiply` `ColorTint` dims/modulates a
  full-color image; passthrough when unset (today's behavior).
- **`ImageIcon`** (Widget-Stub Phase 2B, tint deferred there) — a `Mask`
  `ColorTint` recolors the icon to `IconProps.tintColor`; a
  `GradientTint` gives a gradient-colored icon. This phase is the
  "raster tint path in the compositor" that Phase 2B's follow-up table
  points to.

### 12.6 Test

A RootWidget / `ImageRenderTest` scene, validated through the
window-scoped `DisplayList` path:

- The same source bitmap drawn four ways: no tint, `Multiply` color,
  `Mask` color (recolor — use a non-white source so it's distinguishable
  from multiply), and a `GradientTint` (linear, `Mask`).
- Confirm `Mask` preserves the source's alpha silhouette while replacing
  rgb; confirm `Multiply` darkens toward the tint; confirm the gradient
  tint varies across the dest rect in local space.
- (Phase 8-gated) a `RoundedRect` filled with a `Mask`-tinted bitmap
  brush, confirming the tint composes with corner radius + border in one
  draw.

Visual verification is mandatory (AGENTS.md §Visual Debugging) — hand a
screenshot to the user.

### Files touched

- `wtk/include/omegaWTK/Composition/Brush.h` — `BitmapTint`
  (`Source` / `Mode`, factories); referenced by the bitmap payload and
  (Phase 8) `BitmapBrush`.
- `wtk/src/Composition/Brush.cpp` — `BitmapTint` factory impls.
- `wtk/include/omegaWTK/Composition/DisplayList.h` — bitmap payload
  `tintColor` → `Core::Optional<BitmapTint> tint`; migrate the
  `Color`-taking `DrawOp::Bitmap` ctor overloads to the implicit
  `BitmapTint(Color)`.
- `wtk/src/Composition/backend/RenderTarget.cpp` — resolve `BitmapTint`
  at the bitmap dispatch site ([~2046](../src/Composition/backend/RenderTarget.cpp:2046));
  `emitBitmapPrimitive` writes `tintMode` / `tintSource` and (gradient)
  marshals stops/geometry via the shared 9.3 helper; authors the `local`
  quad varying.
- `wtk/src/Composition/backend/shaders/compositor.omegasl` — extend
  `OmegaWTKBitmapDrawParams` (`tintMode`, `tintSource`, gradient params)
  and `OmegaWTKBitmapVertex`/`RasterData` (`local` varying); Mask arm +
  gradient-eval call in `bitmapFragment` (via `compositor_sdf.omegaslh`).
- `wtk/src/Composition/backend/shaders/compositor_sdf.omegaslh` — reuse
  the 9.3 / 11.3 shared gradient-eval helper (no new helper if 9.3 landed).

### Dependencies / sequencing

- **Color + Multiply:** independent — the path already ships; only the
  `BitmapTint` model refactor (12.1) touches it.
- **Mask mode (12.2):** independent of gradients; small shader + uniform
  change.
- **Gradient tint (12.3):** soft dep on **Phase 9.3** (SDF-native
  gradient evaluator); texture-path fallback if 9.3 is not yet in.
- **Brush tint (12.4):** hard dep on **Phase 8** (bitmap brushes).
- Op-type-agnostic — independent of the compositor op wrapper.

---

## Phase 13 — Gradient text

**Goal:** Fill text with a **Gradient** instead of a solid color. Promoted
from Phase 7 Future now that its prerequisite has landed: **universal
cross-platform MSDF glyph rendering** — msdfgen + WTK's own
[`TextLayoutEngine`](done/Text-Layout-Engine-Plan.md) (shipped, all three
platforms; the platform APIs are used only for font discovery, outline
extraction, shaping, and fallback). With every glyph rendered as an MSDF
coverage mask, gradient fill on glyphs is a **uniform-evaluation
problem** — identical in shape to the SDF-native gradient (Phase 9.3), the
bitmap gradient tint (Phase 12), and `GradientGlass` (Phase 11): evaluate
the gradient color per-fragment and mask it by the coverage the primitive
already computes.

> **Note on sources.** `Direct-To-Drawable-And-SDF-Plan.md` §6.7 (the old
> `GlyphRun::shape()` / per-platform-atlas description) is **superseded**
> by the WTK-owned `TextLayoutEngine`; this phase is written against the
> shipped engine, not that stale section.

Like Phases 8–12 this is **op-type-agnostic** — it lives in the text-run
payload, `RenderTarget.cpp`'s `emitTextSubRun`, and `msdfTextFragment`,
all reading payloads. **No new `DrawOp` variant** — text already emits
`DrawOp::TextRun`; the gradient rides its existing payload.

### Current state — coverage mask, constant-color fill

The shipped text path is one constant away from gradient text.
`Canvas::drawText` runs `TextLayoutEngine::layout(...)` →
`Font::ensureGlyphsResident(...)` → emits a `DrawOp::TextRun`; the backend
then:

- `LayoutResult` gives per-glyph **canvas-space** positions
  (`ShapedGlyph{ glyphId, resolvedFont, canvasX, canvasY }`) plus
  `lineBaselines` ([TextLayoutEngine.h:34](../include/omegaWTK/Composition/TextLayoutEngine.h:34)).
- `DrawOp::TextRun` carries `{ subRuns, rect, Composition::Color color }`
  ([DisplayList.h:199](../include/omegaWTK/Composition/DisplayList.h:199))
  — one sub-run per resolved atlas (fallback faces get their own).
- `emitTextSubRun` ([RenderTarget.cpp:1268](../src/Composition/backend/RenderTarget.cpp:1268))
  authors one 6-vertex quad per resident glyph from the Skia-style
  `AtlasGlyph` metrics (`fLeft`/`fTop`/`fWidth`/`fHeight`,
  [GlyphAtlas.h:74](../src/Composition/backend/GlyphAtlas.h:74)):
  `minX = penX + fLeft`, `minY = penY - fTop`, etc. — into `v_buffer_text`
  (slot 12), fill color in `textParams` (slot 13).
- `msdfTextFragment` computes coverage from the median MSDF distance
  (`smoothstep(0.5±aa)`) and emits `rgb = textColor.rgb`,
  `a = textColor.a × coverage`
  ([compositor.omegasl:176](../src/Composition/backend/shaders/compositor.omegasl:176)).
  Its params struct already reserves an `outlineParamsReserved` slot
  ([compositor.omegasl:159](../src/Composition/backend/shaders/compositor.omegasl:159))
  — deliberate uniform room for exactly this class of extension.

So the *coverage* is done; only the *fill* is a constant. Gradient text
replaces that constant with an analytic gradient evaluation.

### 13.1 Coordinate space — run-relative, not glyph-relative (the decision that matters)

A gradient across text must span the **text run's bounding rect**, not
each glyph quad. If the gradient parameter were computed in per-glyph
local space, every letter would show the *entire* gradient and look
identical — the classic wrong result. Instead, each glyph vertex carries
its position in **run-local gradient space** (`[0,0]`..`[1,1]` across the
gradient rect), and the fragment evaluates the gradient there.

- **Default gradient rect = the `TextRun`'s bounding rect**
  (`textRunParams.rect`), so the gradient flows continuously across the
  whole string — **across line breaks and across sub-runs** (fallback
  faces), because every sub-run and every line
  (`LayoutResult::lineBaselines`) is positioned in one canvas-space frame
  and shares the same run rect. This is the SVG / CSS
  `background-clip: text` behavior callers expect.
- **Optional explicit gradient box.** A caller that wants the text
  gradient to align with a sibling shape's gradient (e.g. a panel and its
  title sharing one gradient) can supply an explicit rect; the glyph
  local coords are then computed against that box instead. Shape-relative
  `[0,1]` gradient geometry (Phase 2.1 / 9.2) makes both cases the same
  evaluation.

`emitTextSubRun` already positions each glyph quad from the layout
engine's canvas-space coords (`ShapedGlyph::canvasX/canvasY` folded into
the pen position, plus the `AtlasGlyph` `fLeft/fTop`), so it computes each
vertex's coord *relative to the gradient box* at quad-author time — no new
CPU state, just an extra varying. (The run-local coord is
canvas-convention-agnostic: it is a normalized `[0,1]` position within the
gradient box, so the Y-down-today / bottom-left-origin-eventually note on
`ShapedGlyph::canvasY` is a one-place flip that does not reach the shader.)

### 13.2 Fill model — `TextFill`

Generalize the text run's `Composition::Color color` to a fill that is
either a color or a gradient, mirroring Phase 12's `BitmapTint` and the
glass `Tint`:

```cpp
struct OMEGAWTK_EXPORT TextFill {
    enum class Source : uint8_t { Color, Gradient };
    Source   source = Source::Color;
    Color    color {};      // Source::Color (today's path)
    Gradient gradient {};   // Source::Gradient (run-relative coords, §13.1)
    Core::Optional<Composition::Rect> gradientBox {}; // unset ⇒ run bbox

    TextFill() = default;
    TextFill(const Color & c) : source(Source::Color), color(c) {}   // back-compat
    static TextFill GradientFill(const Gradient & g,
                                 Core::Optional<Composition::Rect> box = {});
};
```

The `TextRun` payload's `color` field becomes a `TextFill fill` (the
implicit `TextFill(Color)` keeps existing emit sites compiling). Authoring
hook on `StyleSheet`: a `textGradient(tag, gradient)` beside the existing
`textColor(tag, color)`, so UIView text elements opt in without a new
draw API.

### 13.3 Shader — evaluate the gradient, mask by coverage (MSDF path)

- **Vertex/varying:** add a `local` field to `OmegaWTKTextVertex` /
  `OmegaWTKTextRasterData` (the glyph vertex's run-local coord, §13.1),
  distinct from the atlas `uv`.
- **Params:** extend `OmegaWTKTextDrawParams` — repurpose the reserved
  slot — with `uint fillSource` and the gradient stops / geometry /
  spread, reusing the **Phase 9.3** uniform layout.
- **Fragment:** in `msdfTextFragment`, compute `coverage` as today, then
  `fill = (fillSource == Gradient) ? evalGradient(raster.local) :
  textColor` via the shared `compositor_sdf.omegaslh` gradient evaluator
  (from 9.3 / 11.3), and emit `rgb = fill.rgb`, `a = fill.a × coverage`.
  Solid color is the `fillSource == Color` branch — byte-identical to
  today.

`emitTextSubRun` marshals the gradient stops/geometry into `textParams`
when `fill.source == Gradient` (same helper Phase 12 / 9.3 use) and writes
the `local` coord per vertex.

### 13.4 Bitmap-fallback path — gradient text via Phase 12 reuse

MSDF is the path for **every platform** now (msdfgen + the WTK layout
engine on macOS, Windows, and Linux alike), so §13.3 is the primary
route universally. The bitmap fallback is the **narrow per-font case**:
`Font::Mode::BitmapFallback` for faces whose outlines can't be extracted
(bitmap-only fonts, some color-emoji faces), decided per font at
construction — *not* a whole-platform gap. `emitTextSubRun` already
branches on `subRun.resolvedFont->mode()`
([RenderTarget.cpp:1319](../src/Composition/backend/RenderTarget.cpp:1319)),
so a Latin-MSDF string with a color-emoji fallback sub-run mixes both in
one `TextRun`.

Gradient fill on the fallback glyph is **not** a new mechanism — it is
exactly **Phase 12's bitmap gradient tint in `Mask` mode**, with the
gradient evaluated in the same run-local space (§13.1): the glyph
bitmap's alpha is the coverage mask, the gradient supplies rgb. So
`TextFill::Gradient` on a fallback sub-run lowers to a
`BitmapTint::GradientTint(g, Mode::Mask)` on the fallback glyph quad,
keeping the gradient continuous with the MSDF sub-runs beside it.
Solid-color fallback already works.

### 13.5 Free once authored: gradient outline / glow

Because §13.3 evaluates a gradient against run-local coords and the MSDF
band gives outline/glow for free (the `dist > 0` band outside the glyph
silhouette — the same `outlineParamsReserved` slot already sits on the
text params), a **gradient outline** or **gradient glow** is the same
evaluator applied to the outline coverage band instead of the fill band —
noted as a natural follow-up, not in scope for Phase 13.

### 13.6 Test

A RootWidget / `TextCompositorTest` scene, validated through the
window-scoped `DisplayList` path:

- A multi-word, multi-line string with a linear `TextFill::Gradient` —
  confirm the gradient flows continuously across words **and line
  breaks** (run-bbox space), not restarting per glyph.
- A radial gradient fill centered on the run.
- A mixed-script string (Latin + CJK) so a fallback sub-run participates
  — confirm the gradient is continuous across the MSDF and fallback
  sub-runs (§13.4).
- Solid-color text unchanged (regression).

Visual verification is mandatory (AGENTS.md §Visual Debugging) — the
"gradient restarts per glyph" failure passes a coverage check but is
obviously wrong on screen; hand a screenshot to the user.

### Files touched

- `wtk/include/omegaWTK/Composition/Brush.h` / `Brush.cpp` — `TextFill`
  (`Source`, `GradientFill` factory).
- `wtk/include/omegaWTK/Composition/DisplayList.h` — `TextRun` payload
  `Composition::Color color` → `TextFill fill`; migrate emit sites via
  the implicit `TextFill(Color)`.
- `wtk/include/omegaWTK/UI/UIView.h` / StyleSheet — `textGradient(tag,
  gradient)` authoring hook beside `textColor`.
- `wtk/src/Composition/backend/RenderTarget.cpp` — `emitTextSubRun`
  authors the per-vertex run-local `local` coord + marshals gradient
  stops/geometry into `textParams` (shared 9.3 helper); fallback sub-run
  lowers `TextFill::Gradient` to a Phase 12 `Mask` gradient tint.
- `wtk/src/Composition/backend/shaders/compositor.omegasl` — `local`
  varying on `OmegaWTKTextVertex`/`RasterData`; extend
  `OmegaWTKTextDrawParams` (`fillSource` + gradient params, reusing the
  reserved slot); gradient-eval branch in `msdfTextFragment` via
  `compositor_sdf.omegaslh`.

### Dependencies / sequencing

- **Hard dep on the shipped WTK text stack** — `TextLayoutEngine`
  ([done/Text-Layout-Engine-Plan.md](done/Text-Layout-Engine-Plan.md)) +
  universal msdfgen MSDF glyph rendering, which provide the coverage mask
  and `TextRun` path this phase fills. Already in on all three platforms.
- **Soft dep on Phase 9.3** (SDF-native gradient evaluator + shared
  `compositor_sdf.omegaslh` helper); texture-path fallback if 9.3 is not
  yet in, same as Phase 12.
- **Fallback gradient text (13.4) depends on Phase 12** (bitmap gradient
  tint, `Mask` mode), only for the narrow `BitmapFallback`-mode faces.
  MSDF-path gradient text (13.3) — the universal path — does not.
- Op-type-agnostic — independent of the compositor op wrapper.

---

## Phase 7 — Future

These items are deferred. They are listed to confirm the Phase 0–6 designs are forward-compatible.

| Item | Depends on | Notes |
|------|------------|-------|
| SDF-native gradient sampling on simple primitives | — | **Promoted to Phase 9.3.** |
| `GradientSpace::Window` (gradient coords in window space, not shape-local) | `DisplayList` transform ops | Without transforms, "window space" is just "shape space + offset" |
| Pattern brush (image/texture tiling) | — | **Promoted to Phase 8 (bitmap brushes).** |
| `BlendMode` on the relevant `DrawOp` / `PushEffect` scope | `DrawOp` blend fields | Extend fragment shader with blend equations. Note: SDF pipeline already has alpha-over blending enabled; color / texture pipelines stay opaque-write |
| Gradient text | — | **Promoted to Phase 13** — MSDF text (§6.7) has landed, so gradient fill on glyphs is now a uniform-evaluation problem (same shape as SDF-native gradients / Phase 12 bitmap tint) |
| Image scale modes (aspect-fit/fill, tiling, source rect) | — | **DONE** — Direct-To-Drawable-And-SDF-Plan §6.6 landed (hardcoded quad + sampler/mipmap upgrade, source-rect sampling, nine-slice, color tint) |
| Text draw options (maxLines, truncation, underline) | — | Text layout engine changes |
| Implicit `FrameBuilder` `TextLayout` cache (LRU keyed on text+font+rect+color+layoutDesc) | Phase 6 | Lets third-party draw callers (no UIView spec wiring) get caching without holding handles. Skipped in Phase 6 because key construction is fiddly (UniString hash, Color tolerance, Rect equality) |
| `TextLayout` VRAM cap with LRU eviction | Phase 6 | Bounds resident GPU texture memory if a UIView retains many transient layouts |
| Effect bounds (subregion blur) | — | Compositor change |

---

## Dependency graph

```
Phase 0:  Foundation cleanup           [DONE]
Phase 0A: Geometry type isolation      [DONE]

Phase 9: Finish gradients (consolidates the old Phase 1 + Phase 2)
    9.1 texture path → 9.2 geometry → 9.3 SDF-native sampler → 9.4 SVG import
    (op-type-agnostic: brush model + RenderTarget.cpp + shaders)

Phase 8: Bitmap brushes (Brush::Type::Bitmap; no new DrawOp variant)
    └─→ shares SDF-native texture sampling with Phase 9.3; reuses §6.6 bitmap sampling

Phase 10: MSDF scalable bitmaps (supersedes nine-slice for shape assets)
    └─→ generalizes §6.7 GlyphAtlas → MSDFAtlas; reuses msdfTextFragment
        reconstruction; composes with Phase 8 bitmap brushes (shape assets only;
        nine-slice / §6.6 stay as the raster fallback)

Phase 11: Glass brushes (Brush::Type::Glass; closed, refraction-first; no new DrawOp)
    └─→ inverts the LayerBlur scratch to capture+blur the backdrop under a shape;
        new compositor_sdf.omegaslh shared header + dedicated glassFragment;
        Clear/Frosted finish + Full/Cheap quality tier (Cheap = one shared
        backdrop/frame, a safety drop for weak/older GPUs via explicit setting
        or frame-budget watchdog — NOT the integrated/discrete bit);
        soft dep on Phase 9.3 (SDF-native gradient eval) for GradientGlass

Phase 12: Bitmap tinting (Color & Gradient; Multiply / Mask; no new DrawOp)
    └─→ extends the shipped solid-color multiply on DrawOp::Bitmap: adds the
        Mask (recolor) semantic + Gradient tint source (reuses Phase 9.3 eval),
        and threads BitmapTint through the Phase 8 bitmap brush. Satisfies the
        Widget-Stub ImageIcon raster-tint follow-up.
        soft dep on 9.3 (gradient tint); hard dep on Phase 8 (brush tint)

Phase 13: Gradient text (TextFill Color|Gradient; no new DrawOp)
    └─→ fills the MSDF glyph coverage with an analytic gradient in run-relative
        space (continuous across glyphs / lines / fallback sub-runs); reuses
        Phase 9.3 eval + compositor_sdf.omegaslh. MSDF is universal cross-platform
        (msdfgen + WTK TextLayoutEngine, all 3 platforms); the narrow per-font
        BitmapFallback faces get gradient text via Phase 12 Mask gradient tint.
        hard dep on shipped WTK text stack; soft dep on 9.3; fallback (13.4) dep on Phase 12

Phase 4: Color improvements (independent — can run in parallel with any phase) [DONE]

Phase 6: Text layout reuse (independent — can run in parallel with any phase)
    └─→ Phase 7 future: implicit FrameBuilder text cache, VRAM-cap LRU eviction
```

Phases 0, 0A, and 4 are done. The remaining high-leverage work is the
brush / pipeline pair: **Phase 9 (finish gradients)** — the largest
unblocker, completing the old Phase 1 + 2 and adding the SDF-native
sampler — and **Phase 8 (bitmap brushes)**, which shares the SDF-native
texture-sampling shape with Phase 9.3 and reuses the §6.6 bitmap
sampling code. Both are op-type-agnostic: they live in the brush model,
`RenderTarget.cpp`, and the shader source, independent of the compositor
op type. Imperative line / polyline / arc / path draws and per-draw
opacity / transform / clip already ride `DrawOp::VectorPath` payloads
and the `DisplayList` state ops (`SetTransform` / `SetOpacity` /
`PushClip` / `PushTransform`). Phase 6 (text layout reuse) is
independent and is the highest-leverage CPU/GPU win for steady-state UI
repaints — every cached `TextLayout` removes a DWrite/CoreText layout
call and a GPU upload from each frame.

---

## File change summary

| File | Phase | Changes |
|------|-------|---------|
| `wtk/include/omegaWTK/Composition/Geometry.h` | 0A | **DONE** — standalone `Point2D` / `Rect` / `RoundedRect` / `Ellipse` structs; owned by Composition submodule |
| `wtk/src/Composition/backend/GeometryConvert.h` | 0A | **DONE** — WTK↔GTE conversion helpers (Composition-private) |
| `wtk/include/omegaWTK/Core/Core.h` | 0A | **DONE** — `<OmegaGTE.h>` removed; geometry typedefs / `Ellipse` deleted |
| `wtk/include/omegaWTK/Core/GTEHandle.h` | 0A | **DONE** — backend-only header for `extern OmegaGTE::GTE gte` |
| ~130 files across all submodules | 0A | **DONE** — mechanical rename to `Composition::Rect` / `Composition::Point2D` / etc. |
| `wtk/CMakeLists.txt` | 0A | **DONE** — OmegaGTE link scoped appropriately |
| `wtk/include/omegaWTK/Composition/Brush.h` | 0, 2, 4 | **0 DONE:** `isColor` / `isGradient` removed. **Phase 4 DONE:** named color constants (`Black`/`White`/`Red`/`Green`/`Blue`/`Yellow`/`Orange`/`Purple`), `fromHSL` / `fromHSV`, `lerp` / `withAlpha` / `lighter` / `darker`. Remaining: `LinearDef`, `RadialDef`, `GradientSpread`, new gradient factories |
| `wtk/src/Composition/Brush.cpp` | 0, 2, 4 | **0 DONE:** boolean init removed. **Phase 4 DONE:** color constants + HSL/HSV factories + arithmetic helpers. Remaining: new gradient factories |
| `wtk/src/UI/FrameBuilder.{h,cpp}` | 6 | `drawText` shim + new `drawTextLayout` emitting a cached `DrawOp::Bitmap` (Phase 6) |
| `wtk/include/omegaWTK/Composition/Path.h` | 0A | **0A DONE** for public signatures. Frame helpers (`RectFrame` / `RoundedRectFrame` / `EllipseFrame`) retained for stand-alone outline use |
| `wtk/src/Composition/Path.cpp` | 0A | **0A DONE** |
| `wtk/include/omegaWTK/Composition/Animation.h` | 0A | **0A DONE** |
| `wtk/src/Composition/backend/RenderTarget.cpp` | 0, 0A, 1, 2, 8, 9, 11 | **0 + 0A DONE.** **SDF spine integration (border, transform, opacity propagation) DONE** via `Direct-To-Drawable-And-SDF-Plan` §6.3 / §6.5 / §3.1. Remaining: gradient compute pass wiring, extended gradient params, bitmap + glass brush dispatch |
| `wtk/src/Composition/backend/RenderTarget.h` | 2 | Update `createGradientTexture` signature |
| `wtk/src/Composition/backend/shaders/compositor.omegasl` | 1, 2 | Implement `linearGradient` / `radialGradient` compute shaders; extend `GradientTextureConstParams` with start/end / center/radii / spread mode. **SDF fragment functions already in** for color fills (`Direct-To-Drawable-And-SDF-Plan` §6.3) |
| `wtk/src/UI/SVGView.cpp` | (SDF §6.5) | **DONE (alongside SDF spine):** SVG `<rect>` / `<rect rx>` / `<circle>` / `<ellipse>` strokes route through `Border` instead of building separate stroked-path frames. **`<line>` / `<polyline>` / `<polygon>` route through the vector-path ops.** |
| `wtk/include/omegaWTK/Composition/FontEngine.h` | 6 | New `TextLayout` handle (text + font + rect + color + layoutDesc → cached glyph layout + `GETexture`) |
| `wtk/src/Composition/TextLayout.cpp` | 6 | **New file** — handle implementation, dirty-flag resolve, lazy `TextRect` build / texture upload |
| `wtk/include/omegaWTK/UI/UIView.h` / `wtk/src/UI/UIView.*.cpp` | 6 | Cache `Core::SharedPtr<TextLayout>` per text-emitting element spec; invalidate on spec change |
| `wtk/src/Composition/backend/GlyphAtlas.{h,cpp}` | 10 | Generalize the §6.7 per-font MSDF cache to a reusable `MSDFAtlas` / `AtlasTile`; glyph atlas becomes a typed instantiation (no text behavior change). Backs MSDF scalable-bitmap tiles |
| `wtk/include/omegaWTK/Composition/Brush.h` / `Brush.cpp` | 8, 10 | **Phase 10:** `ScalableImage` source; `Brush::Type::Bitmap` MSDF dispatch arm (shape assets only) |
| `wtk/src/UI/FrameBuilder.{h,cpp}` (Phase 10) | 10 | `ScalableImage` standalone-image entry (direct nine-slice replacement); `NineSliceInsets` / nine-slice image path marked legacy / raster fallback |
| `wtk/src/Composition/backend/shaders/compositor.omegasl` (Phase 10) | 10 | MSDF arm in `sdfFragment` (compose-with-shape: intersect MSDF coverage with corner radius / border) and `bitmapFragment` (standalone), reusing the `msdfTextFragment` median → `smoothstep` reconstruction |
| `wtk/include/omegaWTK/Composition/DisplayList.h` | 12 | Bitmap payload `tintColor` → `Core::Optional<BitmapTint> tint`; migrate the `Color`-taking `DrawOp::Bitmap` ctors to the implicit `BitmapTint(Color)` |
| `wtk/include/omegaWTK/Composition/Brush.h` / `Brush.cpp` (Phase 12) | 12 | **New** `BitmapTint` (`Source` Color/Gradient, `Mode` Multiply/Mask, factories); shared by the bitmap payload and the Phase 8 `BitmapBrush` |
| `wtk/src/Composition/backend/RenderTarget.cpp` (Phase 12) | 12 | Resolve `BitmapTint` at the bitmap dispatch (~2046); `emitBitmapPrimitive` writes `tintMode`/`tintSource` + authors the `local` quad varying + marshals gradient stops/geometry (shared 9.3 helper) |
| `wtk/src/Composition/backend/shaders/compositor.omegasl` (Phase 12) | 12 | Extend `OmegaWTKBitmapDrawParams` (`tintMode`/`tintSource`/gradient params) + `OmegaWTKBitmapVertex`/`RasterData` (`local` varying); Mask arm + gradient-eval call in `bitmapFragment` |
| `wtk/include/omegaWTK/Composition/Brush.h` / `Brush.cpp` (Phase 13) | 13 | **New** `TextFill` (`Source` Color/Gradient, `GradientFill` factory, optional gradient box) |
| `wtk/include/omegaWTK/Composition/DisplayList.h` (Phase 13) | 13 | `TextRun` payload `Composition::Color color` → `TextFill fill`; migrate emit sites via implicit `TextFill(Color)` |
| `wtk/include/omegaWTK/UI/UIView.h` / StyleSheet (Phase 13) | 13 | `textGradient(tag, gradient)` authoring hook beside `textColor` |
| `wtk/src/Composition/backend/RenderTarget.cpp` (Phase 13) | 13 | `emitTextSubRun` authors per-vertex run-local `local` coord + marshals gradient params (shared 9.3 helper); fallback sub-run lowers `TextFill::Gradient` to a Phase 12 `Mask` gradient tint |
| `wtk/src/Composition/backend/shaders/compositor.omegasl` (Phase 13) | 13 | `local` varying on `OmegaWTKTextVertex`/`RasterData`; extend `OmegaWTKTextDrawParams` (`fillSource` + gradient params, reusing the reserved slot); gradient-eval branch in `msdfTextFragment` |

---

## References

- Core type definitions: `wtk/include/omegaWTK/Core/Core.h` (typedefs at lines 101–122)
- GTE geometry originals: `gte/include/omegaGTE/GTEBase.h` (lines 319–363)
- Current Brush/Color/Gradient: `wtk/include/omegaWTK/Composition/Brush.h`, `wtk/src/Composition/Brush.cpp`
- Compositor op model: `wtk/include/omegaWTK/Composition/DisplayList.h` (`DrawOp` / `DisplayList`); `wtk/src/UI/FrameBuilder.h`
- Path API: `wtk/include/omegaWTK/Composition/Path.h`
- Animation API: `wtk/include/omegaWTK/Composition/Animation.h`
- Backend rendering: `wtk/src/Composition/backend/RenderTarget.cpp`, `RenderTarget.h`
- UIView (general UI consumer): `wtk/src/UI/UIView.cpp`
- SVGView (vector rendering consumer): `wtk/src/UI/SVGView.cpp`
- Shader source:  `wtk/src/Composition/shaders/compositor.omegasl`
