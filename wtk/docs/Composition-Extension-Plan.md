# Composition Extension Plan

Consolidated plan for extending the Canvas, Brush, Color, and Gradient APIs in `omegaWTK/Composition`. Supersedes the separate Brush and Canvas extension proposals.

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

## Compositor op model: the `Canvas` → `DrawOp` / `DisplayList` shift

This plan was written against the **`Canvas` / `VisualCommand` /
`CanvasFrame`** recording model. That model is being retired by
[UIView-Render-Redesign-Plan.md](UIView-Render-Redesign-Plan.md):

- Tier 3 (Phases 3.8 / 3.9, both **DONE** as of 2026-05-21) collapsed
  the *N* per-view `Canvas` instances into a single window-scoped one
  and deleted `CanvasView`. There is no longer a per-view paint
  device; every view appends to one per-window `DisplayList<DrawOp>`
  via the `FrameBuilder`.
- Tier 4 deletes `Canvas`, `VisualCommand`, `CanvasFrame`, and
  `CanvasEffect` outright. `DrawOp` *is* the new compositor op type
  (one record per primitive, fill + border consolidated, soft shadow
  as its own SDF op), and `DisplayList` (one per window per frame)
  replaces `CanvasFrame`. The backend `BackendRenderTargetContext::renderToTarget`
  switch is rewritten to dispatch on `DrawOp` instead of
  `VisualCommand`.

**What this means for the work below.** The two are decoupled along a
clean seam:

- **Backend rasterization is op-type-agnostic.** The SDF pipeline, the
  tessellation + texture pipeline, the gradient compute pass, the
  bitmap blit, and the MSDF text path all read *payload structs*
  (brush, rect, border, gradient params, texture handle). `DrawOp`
  mirrors `VisualCommand` field-for-field (Render-Redesign §2 Tier 2
  cross-cutting decision), so the gradient and bitmap-brush work in
  this plan — which lives in the brush model, the shader source, and
  `RenderTarget.cpp`'s rasterization — is **unchanged** by the op-type
  swap. Author it against the payload, not against `Canvas`.
- **Imperative `Canvas` method phases are superseded.** Phase 3
  (`drawLine` / `drawPolyline` / `drawArc` / unified `drawPath` /
  `DrawOptions`) and Phase 5 (Canvas save/restore/transform/clip
  state stack) added *imperative methods to the `Canvas` class*. With
  `Canvas` deleted, those capabilities are expressed as **`DrawOp`
  variants and `PaintContext` state** instead:
    - The state stack already exists at the type level —
      `DisplayList` carries `PushClip` / `PopClip` / `PushTransform` /
      `PopTransform` (Render-Redesign Phase 2.4) and `SetTransform` /
      `SetOpacity` ops. Phase 5's "reuse the per-element transform /
      opacity machinery" guidance lands here directly.
    - `drawLine` / `drawPolyline` collapse into `DrawOp::VectorPath`
      (they already did at the SVGView level); `drawArc` and unified
      fill+stroke `drawPath` are `DrawOp::VectorPath` payload shapes.
    - `DrawOptions` (per-draw opacity / blend mode) becomes fields on
      the relevant `DrawOp` variants or a `PushOpacity` / `PushEffect`
      scope, not a `Canvas`-level argument.
  Treat Phases 3 and 5 as **design notes for the capabilities**, not
  as a `Canvas`-API surface to build. Where they are already marked
  DONE (border consolidation, `drawLine` / `drawPolyline`), that work
  shipped through the SDF spine and survives the op swap untouched.

The phases that matter going forward and are genuinely independent of
the op-type change are the **brush / pipeline** ones: gradients
(Phase 1 / 2, consolidated by Phase 9 below) and the new **bitmap
brushes** (Phase 8 below).

---

## Current state snapshot

### What works

- `Brush::Type` enum (`Color`, `Gradient`, `None`) is the single dispatch source. Legacy `isColor`/`isGradient` booleans are gone (Phase 0 done).
- `GradientBrush()` header/source signature mismatch is fixed.
- `create32Bit` typo is fixed.
- Canvas exposes border overloads on `drawRect`, `drawRoundedRect`, `drawEllipse`.
- `setBackground()` and `clear()` exist on Canvas.
- Color solid-fill rendering works for all shape primitives and paths.
- `Path` supports `addArc`, `addLine`, `goTo`, `close`, stroke width, and a per-path brush.
- **`Composition/Geometry.h`** owns the public 2D geometry vocabulary; `Core/Core.h` no longer includes `<OmegaGTE.h>`; `Core/GTEHandle.h` is the backend-only handle (Phase 0A done).
- **Border consolidation:** Rect / RoundedRect / Ellipse with a color border emit one `VisualCommand` and render via one SDF draw — the prior `RectFrame` / `RoundedRectFrame` / `EllipseFrame` side-emission from `drawRect` / `drawRoundedRect` / `drawEllipse` is gone (Direct-To-Drawable-And-SDF-Plan §6.5). The frame helpers remain in `Path.h` for clients that explicitly want a stand-alone outline path.

### What doesn't work or is incomplete

| Issue | Location | Impact |
|-------|----------|--------|
| Gradient compute shader is commented out | `compositor.omegasl` (`linearGradient`), `RenderTarget.cpp` const buffer write | Gradient brushes enter the texture pipeline but produce no texture — effectively broken. SDF fragment shader does not yet sample gradient textures either |
| Gradient has only `float arg` (angle or radius) | `Brush.h:62` | No start/end points for linear, no center/focus for radial, no elliptical radii |
| No spread mode on gradients | — | Out-of-range stops clamp implicitly |
| No per-draw opacity on Canvas (top-level) | — | `setElementOpacity` exists per-VisualCommand but there is no Canvas-level `DrawOptions`. SVGView hacks opacity into `Color.a` |
| No `drawLine` / `drawPolyline` on Canvas | — | SVGView builds `Path` objects for `<line>` and `<polyline>` as a workaround |
| No canvas transform stack | — | `setElementTransform` exists per-element but no save/restore stack for nested zoom / pan / rotation |
| `SharedPtr<Brush>&` parameter style | All Canvas draw methods | Can't pass temporaries; forces callers to name every brush variable |
| Path-level fill+stroke unification at the Canvas API | `Canvas::drawPath` | Backend already supports both via `VisualCommand::Data::pathParams` (`brush` + `fillBrush`); Canvas-side overload still uses single-brush convention. Phase 3.0 below |
| Raw `OmegaGTE::` types still appear in `Path.h` / `Canvas.h` internal fields | `Path.h` `Segment::path` (`GVectorPath2D`), `Canvas.h` `pathParams.path`, `Canvas.h` `setElementTransform(Matrix4x4)` | Public signatures partially migrated; some internal fields still expose the GTE types via forward decls |

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
A few internal struct fields (`Path::Segment::path`,
`VisualCommand::Data::pathParams.path`,
`VisualCommand::Data::transformMatrix`) still hold raw GTE types
behind forward declarations — that's working as intended for the
implementation-detail boundary. See "What doesn't work" in the
snapshot for the residue.

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

### 3.0 Unified `drawPath` with explicit fill + stroke brushes

**Goal:** Collapse the current split between "stroked path" and "filled path" into a single `drawPath` call that takes both a fill brush and a stroke brush as separate parameters. Either brush may be null to skip that component.

#### Current state

`Canvas::drawPath(Path &)` reads brush + stroke width from the `Path` itself (`path.impl_->pathBrush`, `path.impl_->currentStroke`). It then branches on `strokeWidth == 0.f`:

- `strokeWidth == 0` → treat the single brush as a fill, emit one `VectorPath` command with `fillBrush = brush`, `strokeBrush = nullptr`.
- `strokeWidth > 0` → treat the single brush as a stroke, emit one command with `strokeBrush = brush`, `fillBrush = nullptr`.

A path can never be stroked *and* filled in one call. Callers that want both (the common SVG case) have to build two `Path` objects or call `drawPath` twice with different brushes — neither is clean, and SVGView currently works around this by duplicating geometry. The stroke-vs-fill toggle is also implicit (zero stroke width == fill), which is a footgun.

The backend (`RenderTarget.cpp` VectorPath dispatch) already accepts separate `brush` + `fillBrush` fields on `VisualCommand::Data::pathParams`, so the underlying command model already supports fill+stroke in one command. Only the Canvas-facing API forces the either/or.

#### 3.0.1 New signature

```cpp
void drawPath(Path & path,
              const Core::SharedPtr<Brush> & fillBrush,
              const Core::SharedPtr<Brush> & strokeBrush = nullptr,
              float strokeWidth = 0.f);
```

Semantics:

- `fillBrush != nullptr` → fill the path's interior with that brush.
- `strokeBrush != nullptr && strokeWidth > 0` → stroke the path outline with that brush at the given width.
- Both set → one draw that fills and strokes (single `VectorPath` command per segment, fill rendered before stroke).
- Both null or stroke width 0 with null fill → no-op.

The `Path`'s own `pathBrush` and `currentStroke` fields become fallbacks for callers that don't pass explicit arguments (see 3.0.3), not the primary API.

#### 3.0.2 Backend dispatch

`VisualCommand` VectorPath already carries `brush` (stroke), `fillBrush`, `strokeWidth`, `contour`, `fill`. The Canvas implementation constructs one command per path segment with:

- `fill = (fillBrush != nullptr)`
- `contour = (strokeBrush != nullptr && strokeWidth > 0)`
- `brush = strokeBrush`
- `fillBrush = fillBrush`

`RenderTarget.cpp` VectorPath handling (lines ~919 stroke, ~928 fill) must be audited to ensure a single command with both `fill` and `contour` true renders fill first, then stroke on top. If the current code assumes mutual exclusion, split it into sequential fill-then-stroke passes within one command.

#### 3.0.3 Migration of existing `drawPath(Path &)` callers

The zero-arg overload stays as a thin shim for backwards compatibility:

```cpp
void drawPath(Path & path); // reads path.pathBrush + path.currentStroke, delegates to new overload
```

Internally it resolves:

- `strokeWidth = path.impl_->currentStroke`
- If `strokeWidth == 0` → `fillBrush = path.pathBrush`, `strokeBrush = nullptr`
- If `strokeWidth > 0` → `strokeBrush = path.pathBrush`, `fillBrush = nullptr`, pass strokeWidth through

This preserves the current `RectFrame` / `RoundedRectFrame` / `EllipseFrame` border path used by `drawRect`/`drawRoundedRect`/`drawEllipse` without touching them.

#### 3.0.4 Simplify shape draw methods [DONE for simple primitives, via SDF spine]

The original framing — "border handling in `drawRect` /
`drawRoundedRect` / `drawEllipse` builds a frame `Path` and delegates
to `drawPath`" — was retired by Direct-To-Drawable-And-SDF-Plan §6.5.
`drawRect` / `drawRoundedRect` / `drawEllipse` now forward the optional
`Border` directly into the `VisualCommand`; the SDF fragment shader
emits fill + stroke coverage from one distance evaluation in a single
draw call. There is no longer a frame-`Path` step for these primitives.

The `RectFrame` / `RoundedRectFrame` / `EllipseFrame` helpers stay in
`Path.h` for clients that genuinely want a stand-alone outline path
(unrelated to a shape's border). Those helpers still go through the
unified `drawPath` flow as in 3.0.1–3.0.3.

#### Files touched

- `wtk/include/omegaWTK/Composition/Canvas.h` — new `drawPath` overload with fill+stroke brushes
- `wtk/src/Composition/Canvas.cpp` — implement new overload, keep legacy single-arg as shim
- `wtk/src/Composition/backend/RenderTarget.cpp` — ensure VectorPath dispatch handles `fill && contour` together (fill then stroke)

### 3.1 `drawLine` [DONE]

```cpp
void drawLine(OmegaGTE::GPoint2D from,
              OmegaGTE::GPoint2D to,
              Core::SharedPtr<Brush> & brush,
              float strokeWidth = 1.f);
```

Implementation: construct a temporary `Path` from `from` to `to`, set brush and stroke, delegate to `drawPath`.

### 3.2 `drawPolyline` [DONE]

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

**Existing primitives:** `Canvas::setElementOpacity(float)` already
emits a `VisualCommand::SetOpacity` that propagates through subsequent
draws (color, texture, and SDF paths all multiply it into the output
alpha — see Direct-To-Drawable-And-SDF-Plan §3.1). The Phase 3.3 work
is to wrap that in a stack-friendly `DrawOptions` API and to ensure
gradient brushes (Phase 1) honor the same opacity multiplier.
Implementation should reuse the existing per-element machinery rather
than introducing a parallel one.

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

## Phase 5 — Canvas state stack

**Goal:** Support transforms and clipping for zoomed, panned, or clipped content.

**Existing primitives that this builds on:**

  - `Canvas::setElementTransform(const Matrix4x4 &)` already emits a
    `VisualCommand::SetTransform` that the backend consumes per-vertex
    on subsequent color / texture / SDF draws (see
    Direct-To-Drawable-And-SDF-Plan §3.1).
  - `Canvas::setElementOpacity(float)` already emits
    `VisualCommand::SetOpacity` with the same propagation contract.

What's missing is the *stack discipline*: `save` / `restore`,
matrix concatenation (translate / scale / rotate compose with the
current transform), and clip rects. The state-stack work below should
push / pop into / out of the existing `currentTransform` /
`currentOpacity` slots on `BackendRenderTargetContext`; no new GPU
state needs to be added.

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

`Canvas::drawText` (`wtk/src/Composition/Canvas.cpp`) does the full
pipeline inline on every call:

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
5. `drawGETexture(...)` — emits the `Bitmap` `VisualCommand`.

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
    friend class Canvas;
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
scale-agnostic at construction; Canvas supplies the current scale on
every `drawTextLayout` and the handle rebuilds on mismatch. See §6.3.

Internally `Impl` owns:

- A lazily-created `TextRect` (via `TextRect::Create(..., renderScale)`,
  built on first resolve with the scale Canvas passes in).
- A lazily-uploaded `TextRect::BitmapRes` (texture + fence).
- The cached `renderScale` the `TextRect` was built against.
- Dirty flags: `layoutDirty_` (text/font/rect/layoutDesc/renderScale
  changed) and `bitmapDirty_` (color changed).

**Resolve:** the first `Canvas::drawTextLayout` call after a mutator
hits the dirty path:

- `layoutDirty_` → tear down the `TextRect`, build a new one,
  re-rasterize (`drawRun`), re-upload (`toBitmap`).
- `bitmapDirty_` only → keep the `TextRect`, re-rasterize the run
  with the new color, re-upload.
- Neither → emit the cached `Bitmap` command directly.

### 6.2 New Canvas API

```cpp
void drawTextLayout(const Core::SharedPtr<TextLayout> & layout);
```

Implementation: resolve the layout (lazy build), then emit a `Bitmap`
`VisualCommand` from the cached `GETexture` + the layout's rect. No
allocation, no upload, no DWrite/CoreText round-trip on the steady
path.

The existing `Canvas::drawText(text, font, rect, color, layoutDesc)`
overload becomes a one-shot convenience:

```cpp
void Canvas::drawText(const UniString & text, ...) {
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
`Composition::ViewRenderTarget::renderScale_`, and is read by Canvas
via `View::getRenderScale()` (`DPI-Aware-Text-Plan.md` §Plumbing).
This is the same plumbing `Canvas::drawText` already uses today
([Canvas.cpp:125](wtk/src/Composition/Canvas.cpp:125)).

`TextLayout` does **not** read `NativeWindow::scaleFactor()` itself
and does **not** hold a back-pointer to View. The Canvas owns the
freshness contract: on every `drawTextLayout`, Canvas reads its owner
View's current `renderScale` and passes it through to the handle's
resolve. The handle compares against the cached scale and treats a
mismatch as a layout-dirty rebuild — same code path as a `setRect` /
`setFont` change. This keeps the handle ignorant of View and lets a
single layout migrate between Canvases / Views with different scales
(rare in practice but free correctness).

```cpp
// Sketch — Canvas-side:
void Canvas::drawTextLayout(const Core::SharedPtr<TextLayout> & layout) {
    const float scale = (ownerView_ != nullptr)
        ? ownerView_->getRenderScale() : 1.f;
    auto bitmap = layout->resolve(scale); // rebuilds if scale differs
    current->currentVisuals.emplace_back(bitmap.s, bitmap.textureFence,
                                         layout->getRect());
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
  - **Implicit cache in Canvas:** Canvas keeps a per-Canvas LRU keyed
    on (text, font, rect, color, layoutDesc) and reuses behind the
    scenes. Smaller blast radius, but cache invalidation becomes
    Canvas's problem and key construction is non-trivial (UniString
    hash, Color tolerance, Rect equality across float jitter).

Opt-in is cleaner — invalidation becomes the caller's explicit
responsibility, which matches how UIView already tracks dirty state
on its specs. The implicit cache is a Phase 7 follow-up if profiling
later shows third-party Canvas users would benefit.

UIView's `pendingTextHandles_` (or similar field) lives on
`UIView::Impl`. The text-style invalidator already runs on
`onSpecChanged`; extend it to call `layout->invalidate()` /
`setText` / `setColor` on the cached handle for the matching element
tag.

### 6.5 Lifetime / GPU resources

`TextLayout::Impl` owns the `TextRect` and the `GETexture`. As long
as one `SharedPtr<TextLayout>` exists, the GPU texture stays
resident. Frames hold the texture via the `Bitmap` `VisualCommand`'s
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
- `wtk/include/omegaWTK/Composition/Canvas.h` — `drawTextLayout`
  declaration.
- `wtk/src/Composition/Canvas.cpp` — `drawText` becomes a thin shim;
  new `drawTextLayout` emits the cached `Bitmap` command.
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
shader source — squarely on the op-type-agnostic side of the
[Canvas → DrawOp shift](#compositor-op-model-the-canvas--drawop--displaylist-shift).

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
closeout, framed for the post-`Canvas` `DrawOp` / SDF reality.

Like Phase 8, this is **op-type-agnostic** — it lives in the brush
model, `RenderTarget.cpp`, and the shader source, all of which read
brush payloads, not `Canvas`.

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

## Phase 7 — Future

These items are deferred. They are listed to confirm the Phase 0–6 designs are forward-compatible.

| Item | Depends on | Notes |
|------|------------|-------|
| SDF-native gradient sampling on simple primitives | — | **Promoted to Phase 9.3.** |
| `GradientSpace::Canvas` (gradient coords in canvas space, not shape-local) | Phase 5 transform stack | Without transforms, "canvas space" is just "shape space + offset" |
| Pattern brush (image/texture tiling) | — | **Promoted to Phase 8 (bitmap brushes).** |
| `BlendMode` enum on `DrawOptions` | Phase 3 DrawOptions | Extend fragment shader with blend equations. Note: SDF pipeline already has alpha-over blending enabled; color / texture pipelines stay opaque-write |
| Gradient text | Phase 1 gradient pipeline + MSDF text | After Direct-To-Drawable-And-SDF-Plan §6.7 lands MSDF text, gradient fill on glyphs becomes a uniform-evaluation problem (same as SDF-native gradients above) |
| Image scale modes (aspect-fit/fill, tiling, source rect) | — | Direct-To-Drawable-And-SDF-Plan §6.6 owns this — moves bitmap to a hardcoded quad with sampler / mipmap upgrade and adds tint / source rect / nine-slice |
| Text draw options (maxLines, truncation, underline) | — | Text layout engine changes |
| Implicit per-Canvas `TextLayout` cache (LRU keyed on text+font+rect+color+layoutDesc) | Phase 6 | Lets third-party Canvas users (no UIView spec wiring) get caching without holding handles. Skipped in Phase 6 because key construction is fiddly (UniString hash, Color tolerance, Rect equality) |
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

Phase 3: Canvas drawing extensions — SUPERSEDED by the Canvas → DrawOp shift
    ├─→ border consolidation + drawLine/drawPolyline: DONE via SDF spine §6.5
    └─→ remaining capabilities expressed as DrawOp::VectorPath payloads / DrawOptions fields

Phase 5: Canvas state stack — SUPERSEDED by the Canvas → DrawOp shift
    └─→ realized as DisplayList PushClip/PushTransform/PushOpacity + SetTransform/SetOpacity ops

Phase 4: Color improvements (independent — can run in parallel with any phase) [DONE]

Phase 6: Text layout reuse (independent — can run in parallel with any phase)
    └─→ Phase 7 future: implicit per-Canvas text cache, VRAM-cap LRU eviction
```

Phases 0, 0A, and 4 are done. With `Canvas` being retired (see the
[Canvas → DrawOp shift](#compositor-op-model-the-canvas--drawop--displaylist-shift)),
the remaining high-leverage work is the brush / pipeline pair:
**Phase 9 (finish gradients)** — the largest unblocker, completing the
old Phase 1 + 2 and adding the SDF-native sampler — and **Phase 8
(bitmap brushes)**, which shares the SDF-native texture-sampling shape
with Phase 9.3 and reuses the §6.6 bitmap sampling code. Both are
op-type-agnostic: they live in the brush model, `RenderTarget.cpp`, and
the shader source, so they are unaffected by the `VisualCommand` →
`DrawOp` swap. Phases 3 and 5 are superseded — their capabilities now
ride `DrawOp` variants and the `DisplayList` state stack rather than a
`Canvas` API. Phase 6 (text layout reuse) is independent and is the
highest-leverage CPU/GPU win for steady-state UI repaints — every
cached `TextLayout` removes a DWrite/CoreText layout call and a GPU
upload from each frame.

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
| `wtk/include/omegaWTK/Composition/Canvas.h` | 0A, 3, 5 | **0A DONE.** **Class scheduled for deletion** (Render-Redesign Tier 4). Phase 3 / 5 remaining items (`drawArc`, unified `drawPath`, `DrawOptions`, save/restore/transform/clip) are **superseded** — realized as `DrawOp` variants / `DisplayList` state ops, not new `Canvas` methods. See the Canvas → DrawOp shift section. |
| `wtk/src/Composition/Canvas.cpp` | 0A, 3, 5, 6 | **0A DONE.** **Border consolidation DONE** (via `Direct-To-Drawable-And-SDF-Plan` §6.5). **Phase 3.1 / 3.2 DONE:** `drawLine`, `drawPolyline` added. **Class scheduled for deletion** (Render-Redesign Tier 4); remaining Phase 3 / 5 capabilities move to `DrawOp` / `DisplayList`, not this file. |
| `wtk/include/omegaWTK/Composition/Path.h` | 0A | **0A DONE** for public signatures. Frame helpers (`RectFrame` / `RoundedRectFrame` / `EllipseFrame`) retained for stand-alone outline use |
| `wtk/src/Composition/Path.cpp` | 0A | **0A DONE** |
| `wtk/include/omegaWTK/Composition/Animation.h` | 0A | **0A DONE** |
| `wtk/src/Composition/backend/RenderTarget.cpp` | 0, 0A, 1, 2, 3, 5 | **0 + 0A DONE.** **SDF spine integration (border, transform, opacity propagation) DONE** via `Direct-To-Drawable-And-SDF-Plan` §6.3 / §6.5 / §3.1. Remaining: gradient compute pass wiring, extended gradient params, top-level `DrawOptions` plumbing, save/restore stack consumption |
| `wtk/src/Composition/backend/RenderTarget.h` | 2 | Update `createGradientTexture` signature |
| `wtk/src/Composition/backend/shaders/compositor.omegasl` | 1, 2 | Implement `linearGradient` / `radialGradient` compute shaders; extend `GradientTextureConstParams` with start/end / center/radii / spread mode. **SDF fragment functions already in** for color fills (`Direct-To-Drawable-And-SDF-Plan` §6.3) |
| `wtk/src/UI/SVGView.cpp` | (SDF §6.5) | **DONE (alongside SDF spine):** SVG `<rect>` / `<rect rx>` / `<circle>` / `<ellipse>` strokes route through `Border` instead of building separate stroked-path frames. **`<line>` / `<polyline>` / `<polygon>` route through `drawLine` / `drawPolyline` (Phase 3.1 / 3.2 follow-up).** |
| `wtk/include/omegaWTK/Composition/FontEngine.h` | 6 | New `TextLayout` handle (text + font + rect + color + layoutDesc → cached glyph layout + `GETexture`) |
| `wtk/src/Composition/TextLayout.cpp` | 6 | **New file** — handle implementation, dirty-flag resolve, lazy `TextRect` build / texture upload |
| `wtk/include/omegaWTK/UI/UIView.h` / `wtk/src/UI/UIView.*.cpp` | 6 | Cache `Core::SharedPtr<TextLayout>` per text-emitting element spec; invalidate on spec change |

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
- Shader source:  `wtk/src/Composition/shaders/compositor.omegasl`
