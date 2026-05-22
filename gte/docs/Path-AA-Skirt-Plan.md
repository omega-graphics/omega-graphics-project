# Path Anti-Aliasing via Silhouette Skirts

## Goal

Make `VisualCommand::VectorPath` draws produce a real one-pixel
`smoothstep` AA band along path silhouettes — fill outlines, stroke band
outer edges, stroke band inner edges — instead of the placeholder
full-coverage stamp the WTK Phase 6.4 landing currently emits.

The shader side is already done: `pathFragment` in
`wtk/src/Composition/backend/shaders/compositor.omegasl` reads a
per-vertex signed `edgeDist`, derives `aa = max(fwidth(edgeDist),
0.0001)`, and computes `coverage = smoothstep(-aa, aa, edgeDist)`. The
WTK side feeds it `+1.0` everywhere today, which resolves to coverage 1.0
everywhere — same visual as the prior flat-color path.

This plan adds the geometry that makes the smoothstep actually smooth.

## Why the placeholder doesn't AA

Every triangulator-emitted path vertex sits exactly on a silhouette:

- Fill-fan vertices are corners of the path outline.
- Stroke-band vertices `a, c` are on the outer band edge; `b, d` are on
  the inner band edge.

With all three corners of every triangle at the same `edgeDist` value,
linear interpolation gives that constant across the entire triangle
interior. `fwidth(edgeDist) → 0`, the `smoothstep` window collapses, and
the fragment shader has nothing to AA against.

To make the gradient exist in screen space, **some vertex of some
triangle has to be on the other side of the silhouette from its
neighbors**. There is no way around this geometrically — linear
interpolation across an all-at-zero triangle is zero.

## Mechanism: silhouette skirts

Along every silhouette edge of the rendered geometry, emit a thin band of
extra triangles that extends a small object-space epsilon outward. The
original interior geometry's vertices carry `edgeDist = +aaSkirtWidth`
(positive = inside the silhouette); the skirt's outermost vertices carry
`edgeDist = -aaSkirtWidth` (negative = outside). Where the skirt straddles
the silhouette, linear interpolation passes through 0 — exactly where
`smoothstep(-aa, aa, edgeDist)` has its transition.

The skirt epsilon (`aaSkirtWidth`) can be arbitrary as long as it is
much smaller than the smallest visible feature, because the fragment
shader's `fwidth(edgeDist)` adapts the smoothstep window to screen
space. `1.0` object-units is a sensible default for typical UI render
scales.

## API surface

### `gte/include/omegaGTE/TE.h`

**Extend `TETriangulationResult::AttachmentData`** with one field:

```cpp
struct AttachmentData {
    FVec<4>  color;
    FVec<2>  texture2Dcoord;
    FVec<3>  texture3Dcoord;
    FVec<3>  normal           = FVec<3>::Create();
    /// Signed distance from this vertex to the nearest silhouette of the
    /// rendered geometry, in object units. Positive = interior, negative =
    /// exterior, 0 on the silhouette. Populated by GraphicsPath2D when
    /// `aaSkirtWidth > 0`; defaults to `+1.f` so consumers that ignore the
    /// field receive full coverage in the WTK path fragment shader.
    float    edgeDistance     = 1.f;
};
```

**Extend `TETriangulationParams::GraphicsPath2D`** with one parameter:

```cpp
static TETriangulationParams GraphicsPath2D(GVectorPath2D & path,
                                            float strokeWidth,
                                            bool  contour,
                                            bool  fill,
                                            float aaSkirtWidth = 0.f);
```

`aaSkirtWidth = 0` (the default) reproduces today's behavior exactly:
no skirts, every emitted vertex carries `edgeDistance = +1.f`. Callers
opt in by passing a positive value; the WTK side will pass `1.f`.

### `gte/src/TE.cpp`

Plumb `aaSkirtWidth` through to the path triangulation case (currently
~lines 690–852). The fill-fan and stroke-band emit loops are unchanged
except they stamp `attachment.edgeDistance = +aaSkirtWidth` on every
vertex. After each loop, when `aaSkirtWidth > 0`, walk the corresponding
silhouette and emit skirt geometry as described in the next section.

## Geometry rules

### Fill-fan skirts

Today the fill emits `pts.size() − 2` triangles, fan-style from
`pts[0]`. The path's silhouette is the closed polyline through `pts[0],
pts[1], ..., pts[N−1], pts[0]`. For each silhouette segment from
`pts[i]` to `pts[i+1]`:

1. Compute the unit outward normal `n` (perpendicular to the segment,
   pointing away from the polygon interior). The polygon-interior side
   is determined by the same winding the fan triangulation uses; flip
   `n` if needed so positive-normal direction means outside.
2. Emit two skirt vertices:
   - `pts[i]    + n · aaSkirtWidth`,  `edgeDistance = −aaSkirtWidth`
   - `pts[i+1]  + n · aaSkirtWidth`,  `edgeDistance = −aaSkirtWidth`
3. Emit two skirt triangles connecting:
   - `pts[i]   (edgeDist = +aaSkirtWidth)`
   - `pts[i+1] (edgeDist = +aaSkirtWidth)`
   - `pts[i+1] + n · aaSkirtWidth (edgeDist = −aaSkirtWidth)`
   - `pts[i]   + n · aaSkirtWidth (edgeDist = −aaSkirtWidth)`

The skirt vertices carry the same color attachment as the fill's
`attachments[1]` — they are the outer edge of the fill, not a separate
brush.

When `params.graphicsPath2DContour` is set the closing segment from
`pts[N−1]` to `pts[0]` gets the same treatment.

### Stroke-band skirts

Each segment quad has corners:

- `a = start + n · halfStroke`  (outer band edge at start)
- `b = start − n · halfStroke`  (inner band edge at start)
- `c = end   + n · halfStroke`  (outer band edge at end)
- `d = end   − n · halfStroke`  (inner band edge at end)

Both edges (`a–c` and `b–d`) are silhouettes of the band. For each
segment, emit:

- **Outer skirt** outside `a–c`, normal direction `+n`:
  - `a' = a + n · aaSkirtWidth`,  `edgeDist = −aaSkirtWidth`
  - `c' = c + n · aaSkirtWidth`,  `edgeDist = −aaSkirtWidth`
  - Two skirt triangles connecting `a, c, c', a'`.
- **Inner skirt** outside `b–d`, normal direction `−n`:
  - `b' = b − n · aaSkirtWidth`,  `edgeDist = −aaSkirtWidth`
  - `d' = d − n · aaSkirtWidth`,  `edgeDist = −aaSkirtWidth`
  - Two skirt triangles connecting `b, d, d', b'`.

The original `a, b, c, d` get `edgeDistance = +aaSkirtWidth`. Skirt
vertices carry the same `attachments[0]` color as the band itself.

That is **4 extra triangles per segment**, in addition to the existing
2 band triangles.

### Stroke-join closure (default: bevel)

Adjacent segments share an endpoint but their outer-edge skirts
diverge: at an acute exterior corner there is a wedge-shaped gap, and
at the interior corner the inner skirts overlap. Without closure the
gap shows as a visible aliasing notch.

**Default behavior — bevel cap:** at each interior endpoint that
connects two segments, emit one degenerate triangle that bridges the
two adjacent skirt edges. Specifically: for the outer skirt of
segments `i` and `i+1` sharing endpoint `p`, emit triangle
`(p, p + n_i · aaSkirtWidth, p + n_{i+1} · aaSkirtWidth)` carrying
`(+aaSkirtWidth, −aaSkirtWidth, −aaSkirtWidth)`. Same construction
mirrored for the inner skirt with negated normals.

This closes the AA band at every join with one triangle per side per
join. It does not produce a true mitered or rounded join; the visible
silhouette still has the bevel shape that the existing band geometry
already produces.

Round / miter joins are out of scope for this plan and live on the
[Triangulation Engine Completion Plan](Triangulation-Engine-Completion-Plan.md).

### End caps (default: butt)

For an open stroke path, the very first and very last segments have
free endpoints with no neighbor to bevel against. Default behavior:
butt caps — emit one skirt rectangle perpendicular to the segment at
each free endpoint, connecting `a` to `b` (start) or `c` to `d` (end)
with two outward-extended vertices at `edgeDist = −aaSkirtWidth`.

Round / square caps are out of scope.

## Geometry budget

| Path | Today | With skirts | Multiplier |
|------|-------|-------------|------------|
| Fill, N-point polygon | N−2 triangles | (N−2) + 2N = 3N−2 | ~3× |
| Stroke, M-segment open path | 2M triangles | 2M + 4M + 2 (caps) + 2(M−1) (joins, both sides) = ~8M | ~4× |
| Stroke, M-segment closed path | 2M triangles | 2M + 4M + 2M (joins, both sides) = 8M | ~4× |

A 32-segment circle stroke goes from 64 to ~256 triangles. A typical UI
icon path is well under 1000 segments, which still triangulates in
sub-millisecond on the CPU path. The SDF rect / rrect / ellipse path
stays much cheaper for the cases it covers, so this only affects actual
`VectorPath` draws.

## WTK-side change

Once the GTE side lands with `aaSkirtWidth = 0` default, the WTK side
flips on. Two edits in
`wtk/src/Composition/backend/RenderTarget.cpp` `VectorPath` case:

1. Pass `aaSkirtWidth = 1.f` (or pull from a `Canvas`-level or
   per-frame setting if we want it tunable later) to the
   `GraphicsPath2D` factory.
2. In the path branch of the vertex-authoring loop, replace the
   `kInteriorEdgeDist` constant with `v.{a,b,c}.attachment->edgeDistance`
   so the skirt's `−aaSkirtWidth` actually reaches the vertex buffer.

No shader changes — `pathFragment` already does the right `smoothstep`
against `fwidth(edgeDist)`.

## Sequencing (suggested split)

**Commit 1 — GTE additions, no behavioral change.**

- Add `AttachmentData::edgeDistance = +1.f`.
- Add `aaSkirtWidth = 0.f` parameter to `GraphicsPath2D` factory and
  the params struct.
- Stamp `edgeDistance = +aaSkirtWidth` (which is `0.f` today, so still
  resolves to "interior" in the shader once the WTK side reads it; or
  leave the default `+1.f` when `aaSkirtWidth == 0` for explicit
  back-compat — either is fine, the field is unused until commit 2).
- Implement the skirt emission paths but only run them when
  `aaSkirtWidth > 0`.
- All existing GTE consumers compile and run identically.

**Commit 2 — WTK turns it on.**

- Pass `aaSkirtWidth = 1.f` from `RenderTarget.cpp`.
- Read `edgeDistance` from the attachment instead of the placeholder
  constant.
- Visual change: SVG outlines, polylines, custom paths all gain
  one-pixel AA along their silhouettes.

Splitting the API extension from the visual change keeps each commit
reviewable and bisectable; if a regression shows up in commit 2 we
revert WTK without touching GTE.

## Open design points

1. **`aaSkirtWidth` units.** Object units is the simplest contract and
   composes with the existing render scale via `fwidth`. Pixel units
   would require knowing the screen-space derivative at triangulation
   time, which is not available CPU-side. Recommendation: object
   units, default 1.0.

   Object units.

2. **Default-on or default-off in the WTK path.** Default-on is the
   right end state; for the initial WTK-flip commit it might be worth
   gating behind a Canvas-level setting until visual diffs are
   reviewed. Recommendation: default-on, no gate. The cost is
   measurable but small and the visual win is the entire point.

   Default-on

3. **Sharing skirt vertices between adjacent segments.** Today's stroke
   band already duplicates `c == a_next` between segments and the
   plan inherits that — every segment gets its own pair of skirt
   vertices even where they coincide with the next segment's. De-dupe
   is a follow-up in the same area as index buffer generation
   (referenced in [Triangulation-Engine-Completion-Plan.md](Triangulation-Engine-Completion-Plan.md)).

4. **Self-intersecting fills.** The fan triangulation already produces
   overlap artifacts for non-simple polygons, and the skirt inherits
   them. Out of scope; fix when fan triangulation gets fixed.

5. **GPU-side path triangulation.** Lives on a separate plan thread.
   When the GPU path triangulation lands, skirts must come with it —
   so the canonical home for skirt logic is the same module as the
   outline walk. That argues for keeping skirt emission inside
   `TE.cpp`'s path case (which both the CPU and future GPU paths can
   share) rather than duplicating it in the WTK vertex authoring.

## Out of scope

- Round / miter stroke joins (defer to TE completion plan).
- Round / square end caps (defer).
- Per-fragment exact distance fields (different mechanism, doesn't
  fit the existing pipeline shape).
- Self-intersection cleanup.
- Index-buffer / vertex de-duplication for skirt geometry.
- GPU-side skirt emission (lands together with GPU-side path
  triangulation).

## Acceptance

- `EllipsePathCompositorTest` and `SVGViewRenderTest` show visibly
  smoother silhouettes at all render scales.
- Stroke joins on an SVG with sharp angles show no aliasing notches
  at the bevel.
- Vertex count for a representative SVG path increases by ≤ 4× and
  CPU triangulation time stays sub-millisecond for typical UI paths.
- `aaSkirtWidth = 0` reproduces today's vertex stream byte-for-byte.
