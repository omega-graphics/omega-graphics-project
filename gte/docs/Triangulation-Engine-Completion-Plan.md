# Triangulation Engine Completion Plan

## Goal

Bring the Triangulation Engine (TE) from its current ~90% functional state to a complete, production-quality system. This plan covers everything that remains after the GEMesh-TextureAssets plan is completed: new primitives, path refinement, normal generation, GPU triangulation parity, index buffer support, LOD control, and the public API polish needed to consider TE "done."

**Prerequisite**: The [GEMesh and Texture Assets plan](GEMesh-TextureAssets-Implementation-Plan.md) is completed first. That plan delivers Phase 1 (texture attachment descriptors, UV assignment for all primitives, normals in AttachmentData), Phase 2 (GEMesh type and builder), and Phase 3 (TextureAsset / MeshAsset loading). This plan builds on top of those results.

## Current State (After GEMesh Plan Completion)

Assuming the GEMesh plan is done, the TE will have:

- **9 primitives**, all triangulating correctly with TypeColor, TypeTexture2D, and TypeTexture3D attachments
- Per-vertex normals on all 3D shapes
- GEMesh abstraction with vertex/index buffers buildable from TETriangulationResult
- TextureAsset and MeshAsset loading

What will still be missing or incomplete:

| Gap | Severity | Detail |
|-----|----------|--------|
| No torus, sphere-with-poles, or capsule primitives | Medium | Common shapes that callers must approximate with paths today |
| No Bezier / arc path segments | Medium | GVectorPath2D/3D is linear-only; curves must be pre-tessellated by caller |
| No stroke joins or caps | Medium | Path strokes are raw perpendicular quads with visible gaps at corners |
| No index buffer generation | Medium | TEMesh stores triangle lists with duplicated vertices; no deduplication |
| GPU triangulation covers only 4 of 9 primitives | Low | Pyramid, Cylinder, Cone, RoundedRect, Path3D always fall back to CPU |
| Hard-coded Path3D stroke width | Low | `TE.cpp:853` ignores params, uses `1.0f` |
| No LOD / adaptive arc control | Low | Global `arcStep` is the only quality knob; no per-primitive or distance-based control |
| `TEMesh::translate/rotate/scale` operates on CPU vertices | Low | No GPU-side transform; fine for current use cases |

---

## Phase 1: New Primitives

### 1.1 Torus

A torus (donut) is parameterized by major radius R and minor (tube) radius r:

```cpp
struct GTorus {
    GPoint3D center;
    float majorRadius;  // R — center of tube to center of torus
    float minorRadius;  // r — tube radius
};
```

Add `TRIANGULATE_TORUS` to `TriangulationType` and `TETriangulationParams::Torus(GTorus &torus)`.

**Triangulation**: Standard parametric mesh — two nested angle loops (theta for major, phi for minor), each controlled by `arcStep`. Produces quads (2 triangles each). Each vertex gets:
- Position: `(R + r*cos(phi))*cos(theta), (R + r*cos(phi))*sin(theta), r*sin(phi)` (translated to center)
- Normal: outward unit normal from tube surface
- UV: `(theta / 2pi, phi / 2pi)` for texture mapping

**Files**: `gte/include/omegaGTE/GTEBase.h` (GTorus struct), `gte/include/omegaGTE/TE.h` (enum + factory), `gte/src/TE.cpp` (triangulation)

### 1.2 Sphere (UV Sphere)

The existing `GEllipsoid` is a 2D ellipse fan (flat, z=0 by default). For proper 3D sphere triangulation with poles, add a dedicated sphere type:

```cpp
struct GSphere {
    GPoint3D center;
    float radius;
};
```

Add `TRIANGULATE_SPHERE` and `TETriangulationParams::Sphere(GSphere &sphere)`.

**Triangulation**: Latitude/longitude rings. Arc-step controlled. Produces triangle fans at poles, quad strips between rings. Each vertex gets:
- Position: spherical to Cartesian
- Normal: unit vector from center
- UV: `(longitude / 2pi, latitude / pi)` — standard equirectangular mapping

The existing `GEllipsoid` remains for 2D ellipse fills (backward compatible). A future pass could unify them or promote GEllipsoid to a proper 3D ellipsoid using the sphere's ring approach with non-uniform radii.

**Files**: `GTEBase.h`, `TE.h`, `TE.cpp`

### 1.3 Capsule

A capsule (stadium solid) is a cylinder with hemispherical caps:

```cpp
struct GCapsule {
    GPoint3D pos;    // center of bottom hemisphere
    float radius;
    float height;    // distance between hemisphere centers (total height = height + 2*radius)
};
```

Add `TRIANGULATE_CAPSULE` and factory method.

**Triangulation**: Reuse cylinder barrel generation for the middle section. Top and bottom are hemisphere ring tessellations (latitude rings from 0 to pi/2 only). Vertices, normals, and UVs follow from the cylinder + sphere patterns.

**Files**: `GTEBase.h`, `TE.h`, `TE.cpp`

---

## Phase 2: Path Curves and Stroke Refinement

### 2.1 Bezier and arc path segments

Currently `GVectorPath2D` / `GVectorPath3D` are linked lists of points with only linear segments. To support curves natively, extend the path segment model:

```cpp
// Add to GTEBase.h alongside existing GVectorPath_Base

template<class Pt_Ty>
struct GPathSegment {
    enum Type { Linear, QuadraticBezier, CubicBezier, Arc };
    Type type = Linear;
    Pt_Ty endPoint;
    // Control points for Bezier, or arc params
    Pt_Ty control1;  // Used by Quadratic + Cubic
    Pt_Ty control2;  // Used by Cubic only
    // Arc parameters (only for Arc type)
    float arcRadiusX = 0, arcRadiusY = 0;
    float arcRotation = 0;
    bool arcLargeFlag = false, arcSweepFlag = false;
};
```

Add a **path builder** API that allows constructing paths with mixed segment types:

```cpp
class GPathBuilder2D {
public:
    void moveTo(float x, float y);
    void lineTo(float x, float y);
    void quadTo(float cx, float cy, float x, float y);
    void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
    void arcTo(float rx, float ry, float rotation, bool largeArc, bool sweep, float x, float y);
    void close();
    GVectorPath2D build();
};
```

Internally, curve segments are flattened to linear approximations during triangulation using a configurable tolerance (derived from `arcStep` or a new `curveTolerance` param). The flattening is recursive subdivision: split the curve at its midpoint until the distance from the midpoint to the chord is below tolerance.

**Backward compatibility**: The existing `GVectorPath2D::append(pt)` API continues to work for linear-only paths. The builder is an addition, not a replacement.

**Files**: `gte/include/omegaGTE/GTEBase.h`, new `gte/include/omegaGTE/GEPathBuilder.h`, `gte/src/TE.cpp` (flattening in path triangulation)

### 2.2 Stroke joins

At path segment junctions, the current implementation leaves gaps (each segment's perpendicular quads are independent). Add join styles:

```cpp
enum class StrokeJoin { Miter, Round, Bevel };
```

Add to `TETriangulationParams::GraphicsPath2D(...)` as an optional parameter (default: `Miter`).

**Implementation in `_triangulatePriv`** (Path2D branch):
- **Miter join**: Extend the edges of adjacent quads until they meet. Clamp by miter limit (default 4.0) — beyond the limit, fall back to Bevel.
- **Bevel join**: Add a single triangle connecting the outer endpoints of adjacent segments.
- **Round join**: Add a fan of triangles forming an arc between the two outer normals. Arc resolution follows `arcStep`.

**Files**: `TE.h` (enum + param), `TE.cpp` (join geometry in Path2D/Path3D triangulation)

### 2.3 Stroke caps

At the start and end of open paths, add cap styles:

```cpp
enum class StrokeCap { Butt, Round, Square };
```

- **Butt** (default): No extension beyond the endpoint. This is what the current code produces.
- **Round**: Semicircle fan at each endpoint, radius = strokeWidth/2.
- **Square**: Extend the stroke by strokeWidth/2 beyond the endpoint as a rectangle.

**Files**: `TE.h`, `TE.cpp`

### 2.4 Fix Path3D stroke width

`TE.cpp:853` hard-codes `const float strokeWidth = 1.f`. Replace with the stroke width from params. This requires threading the stroke width through the `GraphicsPath3D` factory method (currently it only takes `pathCount` and `paths`):

```cpp
static TETriangulationParams GraphicsPath3D(
    unsigned vectorPathCount,
    GVectorPath3D * const vectorPaths,
    float strokeWidth = 1.f);
```

**Files**: `TE.h`, `TE.cpp`

---

## Phase 3: Index Buffer Generation

### 3.1 Vertex deduplication in TEMesh

Currently, `TEMesh::vertexPolygons` stores triangles with full vertex data per corner — shared vertices are duplicated. For a rectangular prism (12 triangles, 8 unique vertices), this means 36 stored vertices instead of 8+36 indices.

Add an optional post-processing pass that deduplicates vertices and produces an index list:

```cpp
struct TEMesh {
    // ... existing vertexPolygons ...

    // Indexed representation (populated by buildIndexed())
    struct IndexedData {
        std::vector<Polygon::/* vertex struct */> vertices;  // deduplicated
        std::vector<uint32_t> indices;
    };
    std::optional<IndexedData> indexedData;

    void buildIndexed(float positionEpsilon = 1e-6f);
};
```

**Algorithm**: Hash vertex positions (quantized to epsilon grid) to detect duplicates. O(n) with a hash map. Preserve attachment data — two vertices at the same position but with different normals or UVs are not duplicated (this is correct for hard edges).

### 3.2 GEMesh builder uses indexed data

When `GEMesh::fromTETriangulationResult` is called and the TEMesh has `indexedData`, build a proper index buffer alongside the vertex buffer. This reduces GPU memory and improves vertex cache utilization.

**Files**: `TE.h`, `TE.cpp`, `GEMesh.h` / `GEMesh.cpp`

---

## Phase 4: Migrate GPU Triangulation Kernels to OmegaSL

The four existing GPU kernels (Rect, Ellipsoid, RectangularPrism, Path2D) are authored three times: inline HLSL in `D3D12TEContext.cpp`, GLSL→SPIR-V bytecode in `VulkanTessSpirv.inc`, and inline Metal C in `MetalTEContext.mm`. Three implementations of identical math means three places to fix every bug, three places that drift apart, and three places where a small parameter change becomes a multi-file edit.

OmegaSL is the project's cross-target shader language and is already the supported path for consumer-facing shaders. There is no reason TE's internal compute kernels should be the corner of the codebase that bypasses it. Migrate first, **then** Phase 5 adds the remaining primitives — write them once in OmegaSL instead of writing the same kernel three times in three languages just to delete it later.

### 4.1 Author the existing kernels in OmegaSL

Create `gte/shaders/te/` containing one `.omegasl` source per existing kernel: `rect.omegasl`, `ellipsoid.omegasl`, `prism.omegasl`, `path2d.omegasl`. Each defines a single compute kernel with the same signature the per-target version uses today (params uniform buffer in, vertex storage buffer out, one thread per output vertex/triangle).

The math is a direct port — vector types, intrinsics, and threadgroup attributes all map across. Anything that doesn't translate cleanly (a Metal-only or HLSL-only intrinsic) should be flagged as a gap in OmegaSL itself and tracked separately, not worked around with backend-specific shims.

### 4.2 Build rule for `te.omegasllib`

Add a CMake step that runs the OmegaSL compiler over `gte/shaders/te/*.omegasl` and emits a single `te.omegasllib` bundle. The bundle is either:

- Embedded into the OmegaGTE binary as a byte array (preferred, no runtime file dependency), or
- Installed alongside the library and loaded by path at TE init.

The choice mirrors how application shader libraries are handled today — pick whichever the existing TE init path already uses to avoid two loading conventions.

### 4.3 Cross-backend loader

Replace the per-backend shader-load paths in `D3D12TEContext`, `MetalTEContext`, and `VulkanTEContext` with a single call to `OmegaGraphicsEngine::loadShaderLibrary("te.omegasllib")` (or the runtime equivalent). The `GEComputePipelineState` objects created from the loaded library live on the `OmegaTriangulationEngine` itself and are shared across backends.

The three `*TEContext` files shrink to thin per-backend glue that owns the pipeline handles and dispatches them — no shader source, no compile calls, no language-specific resource layout code beyond what `GEComputePipelineState` already abstracts.

### 4.4 Delete per-target kernel sources

After 4.3 lands and tests pass:

- Remove the inline HLSL string literals and `D3DCompile` calls from `gte/src/d3d12/D3D12TEContext.cpp`.
- Remove the inline Metal C source strings and `newLibraryWithSource:` calls from `gte/src/metal/MetalTEContext.mm`.
- Delete `gte/src/vulkan/VulkanTessSpirv.inc` entirely. Remove the glslc generation step from CMake.

This is a deletion-only step with no behavior change — it just locks in that there's only one source of truth for these kernels going forward.

### 4.5 Optional runtime-compilation path

When `RUNTIME_SHADER_COMP_SUPPORT` is enabled, allow TE to load the `.omegasl` sources directly via `OmegaSLCompiler` instead of the prebuilt `te.omegasllib`. This is the dev/iteration path — edit a kernel, re-run the test, no rebuild. When the flag is off (release/embedded builds), only the prebuilt library is loaded.

### 4.6 Validation

`GPUTessTest` already covers Rect on all three backends. Extend it to cover Ellipsoid, RectangularPrism, and Path2D. The pre-migration and post-migration outputs must be byte-identical for the same inputs — the math hasn't changed, only the language. Any divergence indicates either an OmegaSL codegen bug or a porting mistake; both must be fixed before the per-target sources are deleted in 4.4.

### Files

| File | Change |
|---|---|
| New `gte/shaders/te/rect.omegasl` | Port from inline HLSL/GLSL/Metal |
| New `gte/shaders/te/ellipsoid.omegasl` | Same |
| New `gte/shaders/te/prism.omegasl` | Same |
| New `gte/shaders/te/path2d.omegasl` | Same |
| `gte/CMakeLists.txt` | OmegaSL build rule for `te.omegasllib`; remove glslc rule |
| `gte/src/TE.cpp` | Load `te.omegasllib` at TE init; share pipeline states across backends |
| `gte/src/d3d12/D3D12TEContext.cpp` | Delete inline HLSL + `D3DCompile`; thin to dispatch glue |
| `gte/src/metal/MetalTEContext.mm` | Delete inline Metal C; thin to dispatch glue |
| `gte/src/vulkan/VulkanTEContext.cpp` | Delete SPIR-V loader; thin to dispatch glue |
| Delete `gte/src/vulkan/VulkanTessSpirv.inc` | No longer used |

---

## Phase 5: GPU Triangulation Parity

Currently 4 of 9 primitives have GPU compute kernels (Rect, Ellipsoid, RectangularPrism, Path2D). The remaining 5 fall back to CPU. Extend GPU coverage. **Phase 4 must land first** — every kernel added here is written once in OmegaSL, never per-target.

### 5.1 Priority assessment

| Primitive | GPU Value | Complexity | Priority |
|---|---|---|---|
| Cylinder | High — many triangles at small arcStep | Medium — barrel + 2 caps | **P0** |
| Cone | High — same reason | Medium — sides + base | **P0** |
| RoundedRect | Medium — recursive rect/arc composition | Medium — multiple sub-primitives | **P1** |
| Pyramid | Low — always 6 triangles | Trivial | **P2** |
| Path3D | Low — same as Path2D but 3D | Low — port 2D kernel | **P2** |

New primitives (Torus, Sphere, Capsule) should get GPU kernels at introduction time.

### 5.2 Implementation

After Phase 4, every kernel is a single OmegaSL source file under `gte/shaders/te/`. For each new primitive, add one `.omegasl` file (e.g. `cylinder.omegasl`, `cone.omegasl`, `roundedrect.omegasl`, `pyramid.omegasl`, `path3d.omegasl`, plus `torus.omegasl`, `sphere.omegasl`, `capsule.omegasl` from Phase 1). Each kernel follows the existing pattern: input params as a uniform buffer, output vertex data to a storage buffer, one thread per output vertex (or per triangle).

The CMake rule from 4.2 picks the new sources up automatically — no per-backend code is touched. The `*TEContext` files only need a new pipeline-state lookup entry per kernel.

### 5.3 GPU texture attachment support

The existing GPU kernels only handle TypeColor. Extend the OmegaSL kernels to output UV coordinates when a texture attachment is present (matching the CPU path's UV mapping). This was listed as a follow-up in the GEMesh plan.

**Files**: `gte/shaders/te/*.omegasl`, plus pipeline registration in `D3D12TEContext.cpp`, `VulkanTEContext.cpp`, `MetalTEContext.mm`

---

## Phase 6: LOD and Adaptive Quality

### 6.1 Per-primitive arc step

Currently `arcStep` is a global setting on the context (`OmegaTriangulationEngineContext::arcStep`). For scenes with mixed detail levels (a large background ellipse vs. a tiny UI button), a global setting forces a tradeoff.

Add an optional per-primitive override:

```cpp
struct TETriangulationParams {
    // ... existing ...
    std::optional<float> arcStepOverride;  // overrides context-level arcStep if set
};
```

In `_triangulatePriv`, use `params.arcStepOverride.value_or(this->arcStep)` wherever `arcStep` is read.

### 6.2 Curve flattening tolerance

For Bezier/arc path segments (Phase 2), add a separate tolerance that controls how finely curves are linearized:

```cpp
struct TETriangulationParams {
    // ... existing ...
    float curveTolerance = 0.5f;  // max pixel error for curve flattening
};
```

This is distinct from `arcStep` (which controls angular step for parametric primitives) and gives finer control over path rendering quality.

### 6.3 Vertex budget hint

For scenarios where the caller wants to cap geometry complexity (e.g. mobile, preview rendering), add an optional vertex budget:

```cpp
struct TETriangulationParams {
    // ... existing ...
    std::optional<unsigned> maxVertexCount;  // if set, adaptively coarsen to stay under budget
};
```

When set, the triangulation adjusts `arcStep` upward (coarser) until the estimated vertex count for the primitive fits within the budget. This is a best-effort hint, not a hard guarantee.

**Files**: `TE.h`, `TE.cpp`

---

## Phase 7: Result Transforms and Utilities

### 7.1 Normals in TEMesh::rotate

`TEMesh::rotate()` currently transforms vertex positions but does not transform normals. After the GEMesh plan adds normals to `AttachmentData`, the rotate method must also apply the rotation matrix to normals (and re-normalize):

```cpp
void TEMesh::rotate(float pitch, float yaw, float roll) {
    auto rotMatrix = rotationEuler(pitch, yaw, roll);
    for (auto &poly : vertexPolygons) {
        // transform positions (existing)
        poly.a.pt = transformPoint(poly.a.pt, rotMatrix);
        // transform normals (new)
        if (poly.a.attachment && poly.a.attachment->normal) {
            poly.a.attachment->normal = transformNormal(poly.a.attachment->normal, rotMatrix);
        }
        // ... same for b, c
    }
}
```

### 7.2 Merge utility

Add a utility to merge multiple `TETriangulationResult` objects into one:

```cpp
TETriangulationResult TETriangulationResult::merge(
    const std::vector<TETriangulationResult> &results);
```

This concatenates all meshes from all results into a single result. Useful when building a scene from multiple triangulated primitives before converting to a single GEMesh with one draw call.

### 7.3 Bounding box computation

```cpp
struct TEBoundingBox {
    GPoint3D min, max;
};
TEBoundingBox TEMesh::computeBoundingBox() const;
TEBoundingBox TETriangulationResult::computeBoundingBox() const;
```

Useful for culling, camera framing, and acceleration structure building (for raytracing integration).

**Files**: `TE.h`, `TE.cpp`

---

## Phase 8: Testing and Validation

### 8.1 Per-primitive visual tests

Extend the existing 2DTest/GPUTessTest apps (or add a new `TECompleteTest`) that renders every primitive type with:
- TypeColor attachment
- TypeTexture2D attachment (with a checkerboard texture to verify UVs)
- For 3D shapes: a directional light to verify normals

### 8.2 CPU vs GPU agreement

Extend `GPUTessTest` to cover all primitives that have GPU kernels (not just Rect). Verify vertex positions match within epsilon.

### 8.3 Index buffer validation

For each primitive, verify that indexed and non-indexed rendering produce identical visual output. Check that index counts match expected values (e.g. RectangularPrism: 8 unique vertices, 36 indices).

### 8.4 Path tests

Test paths with:
- Linear segments (existing)
- Quadratic and cubic Bezier segments (new)
- Arc segments (new)
- Each join style (Miter, Round, Bevel)
- Each cap style (Butt, Round, Square)
- Degenerate cases: zero-length segments, coincident control points, 180-degree turns

### 8.5 New primitive tests

Torus, Sphere, Capsule: verify correct topology (no T-junctions, no degenerate triangles at poles), correct normals (dot with outward direction > 0), and UV continuity.

**Files**: `gte/tests/` (new or extended test apps per backend)

---

## File Change Summary

| File | Changes |
|---|---|
| **Public API** | |
| `gte/include/omegaGTE/GTEBase.h` | Add `GTorus`, `GSphere`, `GCapsule` structs; extend path segment model |
| `gte/include/omegaGTE/TE.h` | Add primitive enums + factories; `StrokeJoin`, `StrokeCap` enums; `arcStepOverride`, `curveTolerance`, `maxVertexCount` params; indexed data in TEMesh; merge + bounding box utilities |
| New `gte/include/omegaGTE/GEPathBuilder.h` | `GPathBuilder2D` / `GPathBuilder3D` with `moveTo`/`lineTo`/`quadTo`/`cubicTo`/`arcTo`/`close` |
| **Implementation** | |
| `gte/src/TE.cpp` | Torus, Sphere, Capsule triangulation; Bezier/arc flattening; stroke joins + caps; Path3D stroke width fix; index buffer generation; per-primitive arcStep; normal rotation; merge + bounding box |
| **GPU Triangulation (OmegaSL)** | |
| New `gte/shaders/te/*.omegasl` | Single-source kernels for all primitives (existing four ported in Phase 4; new ones added in Phase 5) |
| `gte/CMakeLists.txt` | Build rule for `te.omegasllib`; remove glslc step |
| `gte/src/d3d12/D3D12TEContext.cpp` | Delete inline HLSL + `D3DCompile`; add new pipeline-state lookups |
| `gte/src/vulkan/VulkanTEContext.cpp` | Delete SPIR-V loader; add new pipeline-state lookups |
| `gte/src/metal/MetalTEContext.mm` | Delete inline Metal C; add new pipeline-state lookups |
| Delete `gte/src/vulkan/VulkanTessSpirv.inc` | No longer used after Phase 4 |
| **Tests** | |
| `gte/tests/` | Per-primitive visual tests, CPU/GPU agreement, index validation, path curve/join/cap tests |

---

## Implementation Order

```
GEMesh Plan (prerequisite — completed first)
 ├─ Phase 1: Texture attachments + UVs for all primitives
 ├─ Phase 2: GEMesh type + builder
 └─ Phase 3: Asset loading
         │
This Plan  │
         ▼
Phase 1 (New Primitives) ─── Torus, Sphere, Capsule
    │
Phase 2 (Paths) ─── Bezier/arc segments, joins, caps, Path3D stroke fix
    │
Phase 3 (Index Buffers) ─── Vertex deduplication, indexed GEMesh
    │
Phase 4 (OmegaSL Migration) ─── Port existing 4 kernels to OmegaSL, delete per-target shader code
    │
Phase 5 (GPU Parity) ─── New OmegaSL kernels for remaining + new primitives, UV output
    │
Phase 6 (LOD) ─── Per-primitive arcStep, curve tolerance, vertex budget
    │
Phase 7 (Utilities) ─── Normal rotation, merge, bounding box
    │
Phase 8 (Testing) ─── Validates everything
```

Phases 1-3 are independent of each other and can be parallelized. Phase 4 (OmegaSL migration) depends on nothing in this plan and can run in parallel with 1-3, but **must land before Phase 5** so the new kernels never get written in HLSL/GLSL/Metal. Phase 5 depends on Phase 1 (new primitives exist before writing GPU kernels) and Phase 4 (the OmegaSL pipeline exists). Phase 6 is independent. Phase 7 depends on the GEMesh plan's normals work. Phase 8 validates all prior phases.
