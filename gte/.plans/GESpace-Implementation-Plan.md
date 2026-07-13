# GESpace Implementation Plan

## Goal

Add a `GESpace` class to OmegaGTE: a coordinate-space manager that places geometry
(GEMeshes and the supported 3D primitives) into a viewport-defined relative space, gives
each placed object a translate / rotate / scale transform authored in **space units**,
and exposes a per-object model→NDC matrix so the object can be drawn in a scene. This is
the transform layer that lets a GEMesh "actually be seen in a scene" instead of only
existing as an untransformed vertex buffer.

**Kreate is the first consumer.** Kreate already has a scene graph (`Scene`, `Object`,
`Mat4 mvp = projection * view * cachedWorld` — see `kreate/src/Scene.cpp:116`) but no
GTE-level notion of a coordinate space that owns the relative-units → NDC conversion.
GESpace becomes that GTE-owned authority; Kreate's `Scene`/`Object` are refactored to
build on it (Phase 5).

## Design Decisions (confirmed)

These three forks were resolved with the developer before writing and govern the whole plan:

1. **Transform output = matrix only.** GESpace composes a per-object model→NDC
   `FMatrix<4,4>` and that matrix *is* the "final translated result." Geometry is **not**
   re-baked on the CPU — GEMesh vertex buffers stay in local space, and the caller's draw
   pipeline (Kreate's MVP) applies the matrix. This rules out mutating GEMesh GPU buffers
   or returning transformed geometry.
2. **GESpace is a GTE facility Kreate builds on.** It lives in OmegaGTE. Kreate's existing
   `Scene`/`Object` are refactored to delegate to it, so there is one transform authority,
   owned by GTE — not two parallel stacks.
3. **Viewport-linear space, no built-in camera.** GESpace maps relative/world units → NDC
   directly through the viewport (orthographic-style: origin + extent + depth range). Any
   view/projection (perspective camera) stays in the caller. This matches "it will take a
   viewport as an argument."

## Current State / Why This Is Needed

- **TE bakes to NDC immediately.** `OmegaTriangulationEngineContext::translateCoords`
  converts every input coordinate to NDC at triangulation time using the effective
  viewport (`TE.cpp:351`). Triangulation output is therefore already in clip space.
- **The existing CPU transforms are crude and space-unaware.** `TEMesh::translate`
  (`TE.cpp:2043`) takes a viewport only to rescale the translation delta into NDC; `TEMesh::rotate`
  (`TE.cpp:2056`) rotates vertices **in NDC space**, which distorts under a non-square
  viewport (a 90° rotation of a square becomes a rectangle). These are unfit for placing
  objects in a scene.
- **`viewportMatrix` is incomplete.** `GTEMath.h:849` returns `scalingMatrix(2/w, 2/h,
  2/farDepth)` — a scale with no origin offset, no `-1` clip shift, and no Y-flip. It maps
  `[0,w] → [0,2]`, not `[0,w] → [-1,1]`, and ignores `GEViewport::x`/`y`. GESpace needs a
  correct, origin-aware space→NDC matrix (the same origin gap flagged in
  Triangulation-Engine-Completion-Plan.md Phase 9.3).
- **GTE has the math already.** `orthographicProjection(left,right,bottom,top,near,far)`
  (`GTEMath.h:806`), `translationMatrix`/`scalingMatrix`/`rotationEuler` (`:746`/`:754`/`:787`),
  and `Quaternion` with `fromEuler`/`fromAxisAngle`/`toMatrix` (`:908`) cover everything
  GESpace composes.

---

## Phase 1: GESpace Core and Space→NDC Matrix — ✅ IMPLEMENTED

Two things were found while implementing Phase 1 that the plan above got wrong. Both were
confirmed empirically (compiled probe against `GTEMath.h`, not inferred), and both were
resolved with the developer. They govern every later phase, so they are recorded here
rather than in the phase that discovered them.

**Finding A — `transformPoint()` was applying the transpose.** `GTEMath.h`'s matrices are
column-major (element `(row r, col c)` is `m[c][r]`, translation in column 3) — that is
what the transform builders emit, what `Kreate::Mat4`'s `float[16]` assumes, and what the
shaders' `pc.mvp * float4(v.pos, 1.0)` requires. But `transformPoint` was written as
`m * pointToVec4(pt)`, and `Matrix::operator*` multiplies the raw storage as if the first
index were the row — so it applied **Mᵀ**. Measured: `transformPoint(translationMatrix(1,0,0),
origin)` returned `(0,0,0)`, and `rotationZ(+90°)` on `(1,0,0)` returned `(0,-1,0)`.
Translation was dropped entirely and rotation ran backwards.

Consequence: `TEMesh::translate` (`TE.cpp:2053`) is a **no-op** today and `TEMesh::rotate`
(`TE.cpp:2066`) spins the wrong way — these are worse than the "crude" the plan called
them, which only strengthens the deprecation below. `transformPoint` is now fixed to apply
`M·v` (with a homogeneous divide, so perspective matrices map correctly too); its only
three callers are those already-deprecated TEMesh methods, which go from broken to merely
superseded. `Matrix::operator*` is **left alone** — it is used repo-wide (Kreate, AQUA) and
changing its order is a separate, much larger refactor. Be aware of what it does: GTE
`A * B` yields the GPU matrix `B·A`, i.e. it reads "apply A first, then B".

**Finding B — depth range is [0,1], not [-1,1].** The plan specified
`z_ndc = 2*z/vp.farDepth - 1` to match `translateCoordsDefaultImpl`. That is the OpenGL
[-1,1] depth range, and **no backend GTE ships clips against it** — Vulkan, D3D12 and Metal
all clip depth to [0,1], so a z just above 0 baked through TE's `z > 0` branch lands at
NDC ≈ -1 and is clipped away entirely. GESpace therefore maps depth to **[0,1]**
(`z_ndc = (z - near)/(far - near)`; near→0, far→1) and does *not* inherit TE's depth bug.
X and Y are unchanged from the plan and remain exactly `translateCoordsDefaultImpl` for an
origin-anchored viewport. (TE's own depth baking is left as-is; fixing it belongs to the
TE plan.)

### 1.1 The class

```cpp
// gte/include/omegaGTE/GESpace.h
class OMEGAGTE_EXPORT GESpace {
public:
    OMEGACOMMON_CLASS("OmegaGTE.GESpace")

    /// Construct a space whose relative units map to NDC through `viewport`
    /// (origin + width/height + near/far depth). Copies the viewport by value;
    /// call setViewport() to re-anchor (e.g. on window resize).
    explicit GESpace(const GEViewport & viewport);

    void setViewport(const GEViewport & viewport);
    const GEViewport & viewport() const;

    /// The linear relative-space → NDC transform derived from the current
    /// viewport. This is GESpace's canonical "coordinate conversion."
    FMatrix<4,4> spaceToNDC() const;

    // Object management arrives in Phases 2-4.

private:
    struct Impl;
    UniqueHandle<Impl> impl;   // std::unique_ptr per repo idiom
};
```

### 1.2 The space→NDC matrix

Build it from the viewport as an origin-aware orthographic map — the correct version of
`viewportMatrix`. Given `GEViewport{x, y, width, height, nearDepth, farDepth}` and the
Y-down / top-left convention already documented in `translateCoordsDefaultImpl`
(`TE.cpp:352`), the space→NDC transform is:

```
x_ndc = 2*(x - vp.x)/vp.width  - 1
y_ndc = 1 - 2*(y - vp.y)/vp.height              // Y-flip: y=0 → +1
z_ndc = (z - vp.nearDepth)/(vp.farDepth - vp.nearDepth)   // [0,1] — see Finding B
```

Implemented via `orthographicProjection(vp.x, vp.x+vp.width, vp.y+vp.height, vp.y,
vp.nearDepth, vp.farDepth)` (note `bottom`/`top` swapped to encode the Y-flip) — its X and
Y rows are exactly right, origin term included. Its **depth row is not usable**: it maps
near→0 but far→**-1** (a sign-flipped [0,1]), matching neither convention, so `GESpace.cpp`
overwrites `m[2][2]` and `m[3][2]` with the [0,1] map. `orthographicProjection` itself is
left untouched — it has no other callers, but it is public GTE math and repairing it is a
separate decision.

Do **not** reuse `viewportMatrix` — it is scale-only and origin-blind; leave it alone (or
deprecate separately) to avoid perturbing other callers.

A degenerate viewport (zero width, height, or depth range) logs an error and yields
identity. A NaN matrix reaching a draw call is far harder to debug at 3am than an identity
one, and a zero-height viewport is a real state (a minimized window).

> This matrix is exactly what Phase 9 of the TE plan makes the CPU path honor per-primitive.
> GESpace expresses the same mapping as a composable matrix instead of baking it into vertices.

**Files**: new `gte/include/omegaGTE/GESpace.h`, new `gte/src/common/GESpace.cpp`,
`gte/include/omegaGTE/GTEMath.h` (`transformPoint` fix, Finding A). **No `gte/CMakeLists.txt`
edit is needed** — it picks up `src/common/*.cpp` via `file(GLOB COMMON_SRCS ...)` at line 39.

---

## Phase 2: Object Model and Transforms

### 2.1 Placed object + handle

An object is a geometry reference plus a model transform (local → space units). Because
the output is matrix-only, the object stores TRS components, never mutated geometry.

```cpp
typedef uint32_t GESpaceObjectID;   // stable handle returned by add*()

struct GESpaceTransform {
    GPoint3D    translation {0,0,0};      // space units
    FQuaternion rotation = FQuaternion::Identity();
    GPoint3D    scale {1,1,1};

    /// T * R * S — local → space. Rotation via Quaternion::toMatrix()
    /// (Triangulation-Engine-Completion-Plan Phase 7.1 mandates Quaternion
    /// rotation over Euler for stored orientation).
    FMatrix<4,4> modelMatrix() const;
};
```

### 2.2 Transform mutators (on the space, keyed by handle)

```cpp
void GESpace::setTranslation(GESpaceObjectID, const GPoint3D &);
void GESpace::translate     (GESpaceObjectID, float dx, float dy, float dz);
void GESpace::setRotation   (GESpaceObjectID, const FQuaternion &);
void GESpace::rotate        (GESpaceObjectID, float pitch, float yaw, float roll); // fromEuler
void GESpace::rotateAxis    (GESpaceObjectID, float ax, float ay, float az, float radians);
void GESpace::setScale      (GESpaceObjectID, const GPoint3D &);
void GESpace::scale         (GESpaceObjectID, float sx, float sy, float sz);
const GESpaceTransform & GESpace::transformOf(GESpaceObjectID) const;
```

Euler helpers route through `FQuaternion::fromEuler` (X→Y→Z, matching `rotationEuler`);
composing rotations multiplies quaternions so repeated `rotate()` calls accumulate
without matrix drift — this is the concrete fix for the NDC-space-rotation distortion in
the old `TEMesh::rotate`.

### 2.3 Retrieve the final result per object

```cpp
/// Composed local → NDC: spaceToNDC() * transformOf(id).modelMatrix().
/// THIS is "the final translated result." Feed it to the draw pipeline as the
/// object's transform (Kreate uses it as its per-object MVP; see Phase 5).
FMatrix<4,4> GESpace::objectTransform(GESpaceObjectID) const;
```

**Files**: `GESpace.h`, `GESpace.cpp`.

---

## Phase 3: Placing GEMeshes

Let callers drop an existing GEMesh into the space and transform it. The GEMesh is
referenced (shared), never copied or rebaked.

```cpp
GESpaceObjectID GESpace::addMesh(const SharedHandle<GEMesh> & mesh);
SharedHandle<GEMesh> GESpace::meshOf(GESpaceObjectID) const;
void GESpace::remove(GESpaceObjectID);
OmegaCommon::Vector<GESpaceObjectID> GESpace::objects() const;
```

This is the direct answer to "GEMesh should be able to be translated/rotated/scaled so we
can see it in a scene": `addMesh` → `translate`/`rotate`/`scale` → `objectTransform` →
draw. We deliberately add **no** transform methods to `GEMesh` itself — under the
matrix-only decision, GEMesh stays an immutable GPU resource and the transform lives on
the space object.

**Files**: `GESpace.h`, `GESpace.cpp`.

---

## External Dependency — Local-Space (Un-baked) Triangulation — ✅ SATISFIED

Placing a *primitive* (Phase 4) requires geometry in **local/space units**, but TE
triangulation used to always bake to NDC (`translateCoords`), so applying `spaceToNDC *
model` on top of already-NDC vertices would double-project. The un-baked triangulation path
GESpace needs is owned by the TE plan —
[Triangulation-Engine-Completion-Plan.md](Triangulation-Engine-Completion-Plan.md)
**Phase 9**, now **implemented**:

- `TETriangulationParams::localSpace` makes the coordinate conversion an identity pass, on
  both the CPU path and every GPU kernel. Phase 4 sets it and gets geometry in authored units.
- TE's depth map was also fixed as part of that phase — it used to divide z<0 by
  `nearDepth`, always 0, so **every 3D primitive came back with half its vertices at −∞**.
  Depth is now the same continuous [0,1] map GESpace uses, so the two agree by construction.
- `TETriangulationParams::viewport` and `::frontFaceRotation` landed too (params-wins
  precedence), so a GESpace-placed primitive can declare its own space and winding.

**Phase 4 is unblocked.** Nothing in Phases 1–3 depended on it (those place existing
GEMeshes, which already carry their own local-space vertices).

---

## Phase 4: Placing 3D Primitives

With local-space triangulation available (TE Phase 9.6), let GESpace place any supported 3D primitive
directly — it triangulates the primitive in local units once, stores the resulting local
geometry (as a TETriangulationResult and/or a GEMesh built from it), and gives it a
transform like any other object.

Supported 3D primitives (the 3D subset of `TETriangulationParams::TriangulationType`;
the 2D shapes — Rect, RoundedRect, the flat Ellipsoid fan, Path2D — are out of scope for
a 3D space):

- RectangularPrism, Pyramid, Cylinder, Cone, Torus, Sphere, Capsule

```cpp
/// Triangulate `params` in local space and place it. `frontFaceRotation`
/// forwards to TE. Requires a TE context because triangulation is device-bound.
GESpaceObjectID GESpace::addPrimitive(
    OmegaTriangulationEngineContext * te,
    const TETriangulationParams & params,
    GTEPolygonFrontFaceRotation frontFaceRotation = GTEPolygonFrontFaceRotation::Clockwise);
```

Whether `addPrimitive` also builds a GEMesh eagerly (via `buildMeshFromTriangulation`) or
stores only the CPU `TETriangulationResult` until the caller asks is a small decision —
default to lazy GEMesh construction and expose `meshOf()` to trigger/return it. Reject
`params` whose type is a 2D primitive with a clear error log.

**Files**: `GESpace.h`, `GESpace.cpp`.

---

## Phase 5: Kreate Integration

Refactor Kreate's `Scene`/`Object` to delegate to GESpace so there is one transform
authority. The mapping is clean because Kreate's `Mat4` is column-major `float[16]`,
memcpy-compatible with `FMatrix<4,4>` (`kreate/include/kreate/Math.h`):

| Kreate concept | GESpace backing |
|---|---|
| `Scene::setProjectionMatrix` (for a 2D/ortho scene) | `GESpace::spaceToNDC()` from the render viewport |
| `Object::setTransform(Mat4)` | `GESpaceTransform` on the corresponding `GESpaceObjectID` |
| `Mat4 mvp = projection * view * world` (`Scene.cpp:116`) | `GESpace::objectTransform(id)` when the space owns the projection (`view = identity`) |

Concretely:

- `App::createMesh` / the primitive-creation entry points register the geometry with a
  GESpace owned by the `Scene` (or renderer), returning a `GESpaceObjectID` that
  `Kreate::Object` stores.
- `Object::setTransform` decomposes to / forwards a `GESpaceTransform` (or Kreate exposes
  translate/rotate/scale directly and drops raw `Mat4` authoring — decide with the Kreate
  owner).
- For a perspective scene, Kreate keeps its own `projection`/`view` and uses GESpace only
  for the model matrix (`objectTransform` with an identity space→NDC, i.e.
  `modelMatrix()`); for a viewport-linear 2D/UI scene, `objectTransform` supplies the full
  MVP. Both are supported because the space→NDC step is just one matrix in the product.

> **Open bug to resolve in this phase (found during Phase 1, not yet fixed).** Because GTE's
> `Matrix::operator*` composes in reverse (`A * B` → GPU `B·A`, see Finding A),
> `Mat4 mvp = projection * view * cachedWorld` (`kreate/src/Scene.cpp:116`) hands the shader
> `world·view·projection` — the reverse of what `pc.mvp * float4(v.pos, 1.0)` needs. It is
> only invisible today where `view`/`projection` are identity. Either flip the composition
> order in Kreate or route it through GESpace, which composes explicitly. Verify against a
> non-identity view before closing this phase.

This phase is scoped in the Kreate module and should get its own short note in a Kreate
`.plans/` doc; list it here as the integration contract, not the Kreate-side breakdown.

**Files**: `kreate/src/Scene.cpp`, `kreate/src/Object.cpp`, `kreate/include/kreate/*.h`
(Kreate-owned).

---

## Phase 6: Testing and Validation

- **Space→NDC matrix** (Phase 1): assert `spaceToNDC()` applied to sample points equals
  `translateCoordsDefaultImpl` for the same viewport (including a non-`(0,0)` origin and
  the `z > 0` branch). Guards against re-introducing the origin-blind `viewportMatrix` bug.
- **Transform composition** (Phase 2): a 90° rotation of a unit primitive about Z, placed
  in a non-square viewport, stays square in space units and only acquires aspect scaling
  through `spaceToNDC` — the concrete regression the old `TEMesh::rotate` failed.
  Round-trip `FQuaternion::fromEuler` → `toMatrix` vs `rotationEuler`.
- **GEMesh placement** (Phase 3): add a mesh, translate/rotate/scale, verify
  `objectTransform` matches the hand-composed `spaceToNDC * T * R * S`.
- **Local-space triangulation** (Phase 4): assert primitive output is in local units
  (a unit sphere spans its literal radius, not NDC) and that `objectTransform` maps it to
  the expected NDC extent.
- **Kreate integration** (Phase 5): a placed primitive renders at the expected screen
  position; visual verification per the AGENTS.md Visual Debugging workflow (hand off a
  screenshot).

**Files**: `gte/tests/` (a `GESpaceTest`), plus Kreate-side verification.

---

## File Change Summary

| File | Changes |
|---|---|
| New `gte/include/omegaGTE/GESpace.h` | ✅ Phase 1: `GESpace` (viewport + `spaceToNDC`). `GESpaceObjectID` / `GESpaceTransform` in Phase 2 |
| New `gte/src/common/GESpace.cpp` | ✅ Phase 1: space→NDC matrix. Object table, transforms, retrieval, primitive placement in Phases 2-4 |
| `gte/include/omegaGTE/GTEMath.h` | ✅ Phase 1: `transformPoint` fixed to apply `M·v` column-major (was applying `Mᵀ` — Finding A) |
| `gte/tests/gespace_test.cpp`, `gte/tests/CMakeLists.txt` | ✅ Phase 1: `omegagte_gespace` unit test (13/13 GTE suite green) |
| ~~`gte/CMakeLists.txt`~~ | Not needed — `file(GLOB COMMON_SRCS src/common/*.cpp)` already picks it up |
| `gte/include/omegaGTE/TE.h` | `TEMesh` / `TETriangulationResult` `translate`/`rotate`/`scale` marked `OMEGA_DEPRECATED` (superseded — see below) |
| `kreate/src/Scene.cpp`, `kreate/src/Object.cpp`, `kreate/include/kreate/*.h` | Delegate transform/space to GESpace (Phase 5; Kreate-owned) |
| `gte/tests/` | `GESpaceTest` |

The local-space (un-baked) triangulation path GESpace consumes is **owned by the TE plan**
([Triangulation-Engine-Completion-Plan.md](Triangulation-Engine-Completion-Plan.md) Phase
9.6), not this plan.

### Deprecation: old TEMesh / TETriangulationResult CPU transforms

`TEMesh::translate/rotate/scale` and the `TETriangulationResult` wrappers are **superseded
by GESpace** and marked `OMEGA_DEPRECATED` (the new portable macro in
`common/include/omega-common/utils.h`). They mutate already-NDC-baked vertices — `rotate`
spins vertices in clip space, so it distorts under a non-square viewport — which is exactly
the defect GESpace's space-unit matrix path fixes. There were no external callers at
deprecation time (only the internal result→mesh delegation, warning-suppressed in
`TE.cpp`), so this is a no-churn soft-deprecate. Removal is deferred until GESpace ships
and any future callers migrate; track it as a follow-up, not part of this plan.

---

## Implementation Order

```
Phase 1 (Core + space→NDC matrix)
    │
Phase 2 (Object model + transforms + objectTransform retrieval)
    │
Phase 3 (Place GEMeshes)     ◄── unblocks GEMesh-in-a-scene immediately
    │
    │   TE Phase 9.6 (local-space triangulation) ── prerequisite, TE-plan-owned
    │        │
    ▼        ▼
Phase 4 (Place 3D primitives — needs TE 9.6)
    │
Phase 5 (Kreate integration)
    │
Phase 6 (Testing)
```

Phases 1–3 are self-contained in GTE and unblock the GEMesh-in-a-scene use case
immediately (place an existing mesh, transform it, retrieve the matrix). Phase 4's only
external dependency is TE Phase 9.6 (local-space triangulation). Phase 5 is the Kreate
consumer and can begin as soon as Phases 1–3 land.
