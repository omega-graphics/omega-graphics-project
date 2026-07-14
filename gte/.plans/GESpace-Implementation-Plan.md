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

## Phase 2: Object Model and Transforms — ✅ IMPLEMENTED

Two divergences from the text below, both recorded because they outlive this phase:

**Finding C — `rotationEuler` composed the wrong order.** 2.2 below says the Euler helpers
route through `FQuaternion::fromEuler` "(X→Y→Z, matching `rotationEuler`)". They did **not**
match. `rotationEuler` was written `rotationZ(roll) * rotationY(yaw) * rotationX(pitch)`,
which *looks* like the standard Rz·Ry·Rx — but `Matrix::operator*` composes in reverse
(Finding A), so it actually applied **Z first and X last**, the opposite of its own
docstring. Measured: for pitch=90°, yaw=90°, `rotationEuler` sent (1,0,0) to (0,1,0) while
`fromEuler` sent it to (0,0,−1). Anything with two non-zero angles silently disagreed.

Fixed to `rotationX(pitch) * rotationY(yaw) * rotationZ(roll)` — the spelling that, under
the reversed operator, actually yields the GPU product `Rz·Ry·Rx` (pitch first, roll last).
It now agrees with `FQuaternion::fromEuler` and with `OmegaGTE::rotate(GVectorPath3D&, ...)`,
which already composed X→Y→Z with direct trig. Only caller was the deprecated
`TEMesh::rotate`. This is the same reversed-`operator*` trap as Finding A; it is now the
third bug traced to it, which is why `GESpace.cpp` routes every composition through one
named `applyThen(first, second)` helper instead of spelling `operator*` at each site.

**`addObject()` was added.** Every mutator in 2.2 is keyed by a `GESpaceObjectID`, but the
only thing that mints one (`addMesh`) is Phase 3 — so Phase 2 as written could not be
exercised at all. `addObject(transform = {})` places a transform-only object (a pure
transform node: an anchor, or a placeholder to be given geometry later). Phase 3's
`addMesh` is this plus a geometry reference.

**Also worth a follow-up, not fixed here (out of scope):** `OmegaGTE::rotate(GVectorPath3D&,
pitch, yaw, roll)` (`GTEBase.h:479`) composes the right *order* but spins the opposite
*direction* from `rotationX`/`fromEuler` — its Rx block computes `y' = cy + sz`, the
transpose of the `rotationX` GTE builds. Path rotation therefore turns the wrong way
relative to every other rotation in the library. It has real path callers, so it is not a
free fix like the ones above; it needs its own decision.

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
/// Composed local → NDC: spaceToNDC() applied AFTER transformOf(id).modelMatrix().
/// THIS is "the final translated result." Feed it to the draw pipeline as the
/// object's transform (Kreate uses it as its per-object MVP; see Phase 5).
FMatrix<4,4> GESpace::objectTransform(GESpaceObjectID) const;
```

Both `modelMatrix()` (T∘R∘S) and `objectTransform()` (spaceToNDC ∘ model) are composed
through `applyThen(first, second)` in `GESpace.cpp` — a named helper that says which matrix
applies first, because writing `a * b` and reading it as "a then b" is exactly how Findings
A and C happened. The tests assert the order by pushing points through the matrix, not by
inspecting it: a point at (1,0,0) under scale×2, +90° about Z, and translate +10 must land
at (10,2,0), which is only true if scale really is applied first and the translation is not
itself scaled and rotated.

**Files**: `GESpace.h`, `GESpace.cpp`, `gte/include/omegaGTE/GTEMath.h` (`rotationEuler`
order fix, Finding C), `gte/tests/gespace_test.cpp`.

**Verification**: `omegagte_gespace` covers the TRS order, `fromEuler`↔`rotationEuler`
agreement, quaternion accumulation (two 45° turns == one 90°; 64 turns come full circle and
stay unit-length, so no drift creeps a scale into the model matrix), `objectTransform` vs a
hand-composed `spaceToNDC(model(p))`, the non-square-viewport rotation regression the old
`TEMesh::rotate` failed (a square stays square in space units and only picks up aspect
through `spaceToNDC`), and loud degradation on unknown handles. 14/14 GTE + 26/26 AQUA
ctest green after the shared-math change.

---

## Phase 3: Placing GEMeshes — ✅ IMPLEMENTED

Let callers drop an existing GEMesh into the space and transform it. The GEMesh is
referenced (shared), never copied or rebaked.

```cpp
GESpaceObjectID GESpace::addMesh(const SharedHandle<GEMesh> & mesh,
                                 const GESpaceTransform & transform = GESpaceTransform());
SharedHandle<GEMesh> GESpace::meshOf(GESpaceObjectID) const;
void GESpace::remove(GESpaceObjectID);
OmegaCommon::Vector<GESpaceObjectID> GESpace::objects() const;
```

This is the direct answer to "GEMesh should be able to be translated/rotated/scaled so we
can see it in a scene": `addMesh` → `translate`/`rotate`/`scale` → `objectTransform` →
draw. We deliberately add **no** transform methods to `GEMesh` itself — under the
matrix-only decision, GEMesh stays an immutable GPU resource and the transform lives on
the space object. Placing one GEMesh twice is legitimate and is how instancing falls out:
two objects, independent transforms, one shared GPU buffer. `remove()` retires a handle
without recycling it, so a stale ID stays loudly invalid rather than quietly addressing
whatever object is added next.

### 3.1 Enabler: `GEMesh::bounds` (local-space AABB)

**Confirmed with the developer before implementing.** Phase 3 as written can place a mesh
but not *see* it: GESpace maps space units → NDC linearly through the viewport, so putting
a loaded asset on screen means choosing a scale, and nothing in GTE knew how big a GEMesh
was. `GEMesh` carries buffers and counts; the CPU vertex stream lives only inside
`MeshParser` and is dropped after upload. Without bounds, the fit scale in any consumer is
a magic number tuned by eye to one asset.

So `GEMesh` gains a `GEMeshBounds bounds` (min / max / `valid`, plus `center()`,
`extent()`, `longestExtent()`), populated at load time by a shared
`geMeshComputeBounds(packed, vertexCount, stride)` that reads the Position attribute (always
the first `float3` of a vertex). All three asset backends already funnel through a packed
CPU float stream, so it is one call site each (Metal's `buildFromPacked` covers both its
Model I/O and FBX paths), and `buildMeshFromTriangulation` accumulates the same box while
it writes vertices. An empty mesh reports `valid == false` rather than a degenerate box at
the origin, which a caller would happily divide by.

Scope: ~90 lines across `GEMesh.h/.cpp` + the three loaders — a small feature, so per
AGENTS.md it gets this note rather than its own phase breakdown. It is not GESpace-specific:
Phase 4 primitives and Kreate need the same thing.

### Finding D — a space viewport's depth range is in SPACE UNITS, not [0,1]

`GEViewport` does double duty and the two jobs disagree on what `nearDepth`/`farDepth`
mean. As the **rasterizer** viewport (`setViewports`) they are the hardware depth range and
must lie within [0,1]. As a **space** viewport (`GESpace`) they are the extent of the space
along Z, in the same units as `width`/`height`.

Reuse the rasterizer's `{near=0, far=1}` for the space and any fit scale sized in pixels
(the tennis racket's is 252×) multiplies the model's Z straight through the far plane: the
mesh is clipped away entirely and the window is blank — with every matrix correct and no
error anywhere. A pixel-space viewport needs a pixel-scaled depth range; MeshAndRaytracing
uses `[-1000, +1000]`, symmetric about 0 so a centered model sits at NDC depth 0.5 with
room on both sides to rotate.

This was caught by the Phase 3 unit test, not by reasoning — the fit assertion failed with
`z = 12060` where `0.5` was expected. It applies to every future GESpace consumer, which is
why it is recorded here: **Phase 4** (a primitive placed in a pixel-space viewport) and
**Phase 5** (Kreate's 2D/UI scene) hit the identical trap. The corollary for the fit itself:
center the model on **all three** axes, not just X/Y.

**Files**: `GESpace.h`, `GESpace.cpp`, `GEMesh.h`, `GEMesh.cpp`, the three
`GE*MeshAsset` loaders, `gte/tests/gespace_test.cpp`.

**Verification**: `omegagte_gespace` grew Phase 3 coverage — the mesh is referenced not
copied (`meshOf(id) == mesh`, and the space holds a reference), one mesh placed twice keeps
independent transforms, `objects()` enumerates in insertion order, `remove()` releases the
mesh reference and retires the handle without recycling it, `addMesh(nullptr)` is refused,
bounds read only the Position attribute (a Position+Normal stride whose normals sit far
outside the position range must not poison the box), an all-negative mesh is not stretched
to the origin, and the fit-to-viewport workflow lands the model's center at NDC (0,0,0.5)
with all eight bounds corners inside the clip volume. 15/15 GTE + 26/26 AQUA ctest green.

**Consumer**: `gte/tests/MeshAndRaytracingTest` — see below.

### Finding E — `geMeshStrideFor` was a second, wrong copy of the buffer layout

Found by *looking at the frame*, which is the only reason it was found at all: with the
mesh correctly placed, the racket rendered as a shredded diamond. It was malformed before
GESpace too — the model happens to span ≈[-0.95, 0.95] × [-0.41, 0.41], already inside the
clip volume, so the pre-GESpace test was drawing the same garbage, just smaller. Nothing in
the plan predicted this and no unit test caught it; it is invisible to anything short of a
screenshot.

**The bug.** `geMeshStrideFor()` computed a vertex's stride by summing its attributes'
component sizes — 12 bytes for Position-only. But a `buffer<T>` element is laid out by the
backend's buffer standard, and under **std430 a `float3` has a 16-byte base alignment**, so
the struct rounds up: the compiled SPIR-V decorates the array `ArrayStride = 16` (verified
by disassembling `meshFunc.spv`, not by reading the spec). The GPU therefore read vertex
*i* at byte `16i` while `MeshParser` had packed it at `12i` — each vertex drifting 4 more
bytes off than the last, reading a blend of its neighbours' components. That shreds the
triangles while roughly preserving the silhouette, which is exactly what it looked like.

**The root cause is a second source of truth.** GTE already had a correct, backend-aware
layout authority — `omegaSLStructStride()` returns 16 for `{float3}` under std430, 12 for
D3D12's scalar `StructuredBuffer`, 16 for Metal's `simd_float3`. `geMeshStrideFor` ignored
it and re-derived the number by hand, wrongly. The shader file asserted the wrong rule out
loud in a comment ("a lone `float3` needs no padding so we're aligned"), which is how it
survived review.

**The fix deletes the disagreement rather than adding a third opinion.** `geMeshStrideFor`
now defers to `omegaSLStructStride`, so it is automatically right on every backend. Format
parsers still emit tightly-packed vertices (the natural thing when walking a file — that is
`geMeshTightStrideFor`), and `geMeshRepackToGPULayout()` re-lays the stream into the GPU
layout once before upload, placing each attribute at the offset reported by a new
`omegaSLStructMemberOffsets()` that mirrors the stride function branch for branch. Where
the two layouts already agree (D3D12), the repack is a straight copy.

**Blast radius is wider than this test:** every `GEMeshAsset`-loaded mesh (glTF, OBJ, FBX)
on **Vulkan and Metal** was being read at the wrong stride. D3D12's scalar layout happens to
match the tight packing, which is likely why it went unnoticed — MeshAndRaytracingTest was
written as a D3D12 test. `buildMeshFromTriangulation` was never affected: it drives a
`GEBufferWriter`, which already applies the standard's align-then-place internally.

**Follow-up (not done here):** `GEMeshDescriptor` combinations beyond Position are now
laid out correctly by construction, but none are exercised by a test. A Position+Normal mesh
(tight 24B → std430 32B, normal at offset 16) is the case most likely to regress; it wants
a unit test against `omegaSLStructMemberOffsets`.

### 3.2 First consumer: MeshAndRaytracingTest

The test loaded an FBX and handed its raw local coordinates to the rasterizer as if they
were already clip space, so the racket (authored around unit scale, ~1.90 × 0.81 × 0.17)
drew as a speck-to-nothing against the clear color. It now places the mesh through GESpace:

- The mesh shader takes the MVP as a push constant (`constant<MeshTransform> pc : 0` +
  `[in pc]` on the `mesh(...)` stage — OmegaSL and the Vulkan backend both already support
  push constants on a mesh stage; `GEVulkan.cpp:2826` maps `OMEGASL_SHADER_MESH` to
  `VK_SHADER_STAGE_MESH_BIT_EXT` in the push range). The vertex buffer moved to slot 1.
- The C++ side builds a `GESpace` from an 800×600 pixel-space viewport (depth `[-1000,
  +1000]`, per Finding D), calls `addMesh`, and fits the model from its own
  `GEMesh::bounds`: uniform scale to 80% of the viewport's shorter axis, centered on all
  three axes. No per-asset constant anywhere.
- `objectTransform(id)` is flattened column-major into the push constant each frame.

The bounds also settled a question that would otherwise have been a guess: the racket is
thin in **Z**, so it already faces the viewer and needs no reorientation — an "upright"
quarter-turn about X, which a Z-up model would want, would have turned it edge-on.

Still open (not GESpace's): the test window's render target has **no depth attachment**, so
the draw runs with no depth test and 389k triangles resolve in dispatch order — back faces
can paint over front ones. Placement and silhouette are unaffected; proper occlusion needs
a depth buffer on `GENativeRenderTarget`.

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

## Phase 4: Placing 3D Primitives — ✅ IMPLEMENTED

Three decisions the text below left open or specified differently, all resolved during
implementation and recorded here because they shape the shipped API:

**Rejecting 2D types needed a public predicate, not a friend.** The plan says "reject
`params` whose type is a 2D primitive," but `TETriangulationParams::type` (and its
`TriangulationType` enum) is **private**, and the only friend is
`OmegaTriangulationEngineContext` — GESpace cannot read it. Rather than make GESpace a
friend (which would leak the private enum layout into `GESpace.cpp`), a public
`bool TETriangulationParams::is3DPrimitive() const` was added (declared in `TE.h`, defined
in `TE.cpp` where the private enum is in scope). It returns true for exactly the seven
solids — RectangularPrism, Pyramid, Cylinder, Cone, Torus, Sphere, Capsule — and false for
everything else (Rect, RoundedRect, the flat Ellipsoid fan, Path2D **and** Path3D). This is
a cleaner, more modular surface than exposing the type, and it is reusable by any future
caller that needs the same 2D/3D question. It puts two files (`TE.h`, `TE.cpp`) outside the
"Files" list below.

**Lazy GEMesh needed an engine-taking `meshOf` overload.** The plan says "expose `meshOf()`
to trigger/return it," but the Phase 3 `meshOf(id) const` is const and takes no engine —
and `buildMeshFromTriangulation` needs an `OmegaGraphicsEngine *` that GESpace does not own
and `addPrimitive` (which only gets a *triangulation* context) never receives. So the lazy
build is triggered by a **new overload** `meshOf(id, engine, desc = {})`: it flattens the
stored `TETriangulationResult` into a GPU buffer once, caches it on the object, and returns
the same handle on every later call (`engine`/`desc` then ignored). The const
`meshOf(id)` is unchanged — it returns the cached/placed mesh or null (a not-yet-built
primitive reads as null, which is not an error). `addPrimitive` therefore builds **no** GPU
buffer; the CPU `TETriangulationResult` is enough to place and inspect the primitive.

**`triangulationOf(id)` exposes the stored CPU geometry.** The plan's "as a
TETriangulationResult **and/or** a GEMesh" is realized as: `addPrimitive` always stores the
`TETriangulationResult` (in local/authored units), `triangulationOf(id)` returns it (or null
for a mesh / transform-only / unknown object), and the GEMesh is the optional lazy artifact
above. This is also the readback a local-units assertion needs (a unit sphere's stored
vertices span its literal radius, never NDC).

**Testing is split by device dependency.** `addPrimitive` triangulation is device-bound — it
needs a live `OmegaTriangulationEngineContext`, which needs a device + render target — so it
cannot run in the pure-CPU `omegagte_gespace` suite. That suite was extended with the parts
that *are* device-free: `is3DPrimitive()` classification across all the shape factories, the
null-context refusal (returns `GESpaceInvalidObject`, adds nothing to the space), and clean
degradation of `triangulationOf` / `meshOf(engine)` on unknown / non-primitive handles. The
on-device assertions Phase 6 calls for — primitive output is in local units, and
`objectTransform` maps it to the expected NDC extent — belong to an on-device consumer (the
same shape as MeshAndRaytracingTest) and are **not** part of this pass; they are the open
verification surface for Phase 4. 16/16 GTE `omegagte_gespace` checks pass; OmegaGTE builds
clean on the Metal host.

---

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

As shipped, the surface is (see the findings above for why each landed this way):

```cpp
/// Triangulate `params` in LOCAL space (forces localSpace on) and place it.
/// `frontFaceRotation` forwards to TE as the winding fallback. Rejects a null
/// context or a non-3D-primitive with an error log + GESpaceInvalidObject.
GESpaceObjectID addPrimitive(
    OmegaTriangulationEngineContext * te,
    const TETriangulationParams & params,
    GTEPolygonFrontFaceRotation frontFaceRotation = GTEPolygonFrontFaceRotation::Clockwise);

/// The stored local-space CPU geometry, or null (mesh / transform-only / unknown).
SharedHandle<TETriangulationResult> triangulationOf(GESpaceObjectID id) const;

/// Build (once) + cache the primitive's GEMesh, then return it. For an addMesh
/// object returns the placed mesh unchanged. Null on unknown/transform-only/null
/// engine/build failure.
SharedHandle<GEMesh> meshOf(GESpaceObjectID id, OmegaGraphicsEngine * engine,
                            const GEMeshDescriptor & desc = GEMeshDescriptor());
```

**Files**: `GESpace.h`, `GESpace.cpp` (the primitive API), plus `TE.h` / `TE.cpp`
(`is3DPrimitive` predicate — see finding) and `gte/tests/gespace_test.cpp` (device-free
coverage).

---

## Phase 5: Kreate Integration — ✅ IMPLEMENTED

The Kreate owner resolved the forks this plan left open, and one of them **widened
GESpace's own surface** (a GTE change, not just a Kreate one), so the decisions are recorded
here where GESpace is specified:

**Decision 1 — the projection moved INTO GESpace.** The plan below said "for a perspective
scene, Kreate keeps its own projection/view and uses GESpace only for the model matrix." The
owner overrode that: GESpace now **owns the camera**. It gained `setViewMatrix(FMatrix)` /
`setProjectionMatrix(FMatrix)` / `clearProjectionMatrix()` (+ `viewMatrix()` /
`projectionMatrix()` accessors), and `objectTransform(id)` composes the full
`projection · view · model` itself — where `projection` is the override if set, else the
viewport-linear `spaceToNDC()`. So a consumer never re-derives the MVP and never meets the
reversed `operator*` doing it. This is backward compatible: with no camera set (view =
identity, no projection override) `objectTransform` reduces to exactly `spaceToNDC · model`,
so Phases 1–4 and their tests are unchanged. The projection is supplied as a matrix
(built with `perspectiveProjection` / `orthographicProjection`), so GESpace does not bake in
a camera model. `perspectiveProjection` already maps depth to [0,1], matching GESpace's
Finding-B choice, so a perspective lens lands inside the clip volume with no extra work.

**Decision 2 — Scene owns a GESpace object table (the plan's "full" option).** Kreate's
`Scene` holds a `GESpace`; `Scene::add` registers each `Object` via `addObject(...)` and
stores the `GESpaceObjectID` on its node; `Scene::render` pushes each object's transform
into its slot (new `GESpace::setTransform(id, GESpaceTransform)` — the setter paired with
`transformOf`) and draws it with `objectTransform(id)`. `Scene::setViewMatrix` /
`setProjectionMatrix` forward into the space. The old hand-rolled `mvp = projection * view *
cachedWorld` is gone, which is what closes the reversed-`operator*` bug below.

**Decision 3 — `Object` is TRS-only.** `setTransform(Mat4)` / `transform()` were removed;
`Object` now exposes `setPosition` / `setRotationEuler` / `setRotationAxis` / `setScale` and
stores a `GESpaceTransform` (read by the Scene through an internal `ObjectAccess` bridge, so
no GTE type leaks onto the public `Object` header). Rotation setters replace, not accumulate.

**The reversed-`operator*` bug (below) was fixed the way the owner specified: Kreate's `Mat4`
is now row-major** (`data[row*4+col]`) with a standard `operator*`, and the row↔column
transpose is isolated to the GTE boundary (`Math.cpp`'s `toFMatrix`, the renderer's
push-constant flatten). `projection * view * world` now reads as normal math and reaches the
shader correct; the MVP itself no longer flows through `Mat4` at all (it comes straight off
`objectTransform()` as a column-major `FMatrix`).

**Deferred (documented, not silent):** parent-child hierarchy. GESpace objects are flat, and
composing a hierarchical world transform correctly needs a GPU-order matrix multiply Kreate
can't spell without re-exposing the reversed `operator*`. `Scene::render` treats every object
as a root and logs a one-time note if a caller actually parents one; true hierarchy is left
to the Scene-model phase (Kreate Engine-Roadmap Phase 6).

**Open verification:** the cube in `kreate/tests/BasicGame.cpp` (rewritten to the TRS API)
renders through the new path, but visual confirmation against a **non-identity view** — the
regression this phase was meant to close — is a screenshot handoff per the AGENTS.md Visual
Debugging workflow, pending at the time of writing.

**Files:** GTE — `gte/include/omegaGTE/GESpace.h`, `gte/src/common/GESpace.cpp`,
`gte/tests/gespace_test.cpp` (camera-composition coverage). Kreate —
`kreate/include/kreate/{Math,Object}.h`, `kreate/src/{Math,Object,Scene}.cpp`,
`kreate/src/renderer/Renderer.{h,cpp}`, new `kreate/src/{MathConvert,ObjectAccess}.h`,
`kreate/tests/BasicGame.cpp`. Kreate-side breakdown:
`kreate/.plans/GESpace-Integration-Plan.md`.

---

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
| New `gte/include/omegaGTE/GESpace.h` | ✅ Phase 1: `GESpace` (viewport + `spaceToNDC`). ✅ Phase 2: `GESpaceObjectID`, `GESpaceTransform`, `addObject`, TRS mutators, `objectTransform`. ✅ Phase 3: `addMesh`, `meshOf`, `remove`, `objects`. ✅ Phase 4: `addPrimitive`, `triangulationOf`, engine-taking `meshOf` overload |
| `gte/include/omegaGTE/TE.h`, `gte/src/TE.cpp` | ✅ Phase 4: public `TETriangulationParams::is3DPrimitive()` predicate (true for the seven solids), so GESpace can reject 2D types without befriending the private `TriangulationType` |
| New `gte/src/common/GESpace.cpp` | ✅ Phase 1: space→NDC matrix. ✅ Phase 2: object table + transforms + retrieval (all composition via one `applyThen` helper). ✅ Phase 3: objects hold a shared GEMesh reference; handles retired, never recycled. ✅ Phase 4: `addPrimitive` (local-space triangulate + store CPU result), `triangulationOf`, lazy `meshOf(id, engine, desc)` cache |
| `gte/include/omegaGTE/GTEMath.h` | ✅ Phase 1: `transformPoint` fixed to apply `M·v` column-major (was applying `Mᵀ` — Finding A). ✅ Phase 2: `rotationEuler` fixed to compose X→Y→Z (was composing Z→Y→X — Finding C) |
| `gte/include/omegaGTE/GEMesh.h`, `gte/src/common/GEMesh.cpp` | ✅ Phase 3.1: `GEMeshBounds` (local-space AABB + `center`/`extent`/`longestExtent`), `geMeshComputeBounds()`, `GEMesh::bounds`; `buildMeshFromTriangulation` populates it. ✅ Finding E: `geMeshStrideFor` now defers to `omegaSLStructStride` (was hand-summing, wrongly); new `geMeshTightStrideFor` + `geMeshRepackToGPULayout` |
| `gte/include/omegaGTE/GTEShader.h`, `gte/src/GTEBase.cpp` | ✅ Finding E: new `omegaSLStructMemberOffsets()` — per-member byte offsets under the same backend-aware standard `omegaSLStructStride` sizes with |
| `gte/src/common/MeshParser.cpp` | ✅ Finding E: parsers emit tight, then the stream is re-laid into the GPU layout once before it is published |
| `gte/src/{vulkan,d3d12,metal}/GE*MeshAsset.{cpp,mm}` | ✅ Phase 3.1: each loader fills `GEMesh::bounds` from the packed stream it already holds (one call site each). ✅ Finding E: Metal's Model I/O path repacks its own tight stream (the other two go through MeshParser) |
| `gte/tests/gespace_test.cpp`, `gte/tests/CMakeLists.txt` | ✅ Phase 1: `omegagte_gespace` unit test. ✅ Phase 3: mesh placement, instancing, handle retirement, bounds, and fit-to-viewport (15/15 GTE suite green) |
| `gte/tests/MeshAndRaytracingTest/main.cpp`, `gte/tests/assets/MeshAndRaytracingTest/meshAndRaytracing.omegasl` | ✅ Phase 3.2: first consumer — push-constant MVP on the mesh stage, fed by `GESpace::objectTransform()`; mesh fitted to the viewport from its own bounds |
| ~~`gte/CMakeLists.txt`~~ | Not needed — `file(GLOB COMMON_SRCS src/common/*.cpp)` already picks it up |
| `gte/include/omegaGTE/TE.h` | `TEMesh` / `TETriangulationResult` `translate`/`rotate`/`scale` marked `OMEGA_DEPRECATED` (superseded — see below) |
| `gte/include/omegaGTE/GESpace.h`, `gte/src/common/GESpace.cpp` | ✅ Phase 5: GESpace owns the camera — `setViewMatrix`/`setProjectionMatrix`/`clearProjectionMatrix` + accessors; `objectTransform` composes `projection·view·model` (projection = override or `spaceToNDC`); new `setTransform(id, GESpaceTransform)` whole-transform setter |
| `kreate/src/{Scene,Object,Math}.cpp`, `kreate/src/renderer/Renderer.{h,cpp}`, `kreate/include/kreate/{Scene,Object,Math}.h`, new `kreate/src/{MathConvert,ObjectAccess}.h`, `kreate/tests/BasicGame.cpp` | ✅ Phase 5 (Kreate-owned): Scene owns a GESpace + forwards view/projection; Object TRS-only storing a `GESpaceTransform`; `Mat4` → row-major + standard `operator*`, transpose isolated to the GTE boundary; renderer takes the MVP as a column-major `FMatrix`. Breakdown: `kreate/.plans/GESpace-Integration-Plan.md` |
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
Phase 1 (Core + space→NDC matrix)                     ✅
    │
Phase 2 (Object model + transforms + objectTransform retrieval)   ✅
    │
Phase 3 (Place GEMeshes + GEMesh::bounds)             ✅ ◄── GEMesh-in-a-scene, shipped
    │        └── consumer: MeshAndRaytracingTest      ✅
    │
    │   TE Phase 9.6 (local-space triangulation) ── prerequisite, TE-plan-owned ✅
    │        │
    ▼        ▼
Phase 4 (Place 3D primitives — needs TE 9.6)          ✅ ◄── addPrimitive shipped (GTE-side)
    │        └── open: on-device local-units / NDC-extent verification (Phase 6)
    │
Phase 5 (Kreate integration)                          ✅ ◄── GESpace owns the camera; Kreate delegates
    │        └── open: visual verification (spinning cube, non-identity view — screenshot handoff)
    │
Phase 6 (Testing)
```

Phases 1–4 are self-contained in GTE and are **done**: an existing GEMesh can be placed,
transformed, and drawn from `objectTransform()` (MeshAndRaytracingTest does exactly that),
and a 3D primitive can now be triangulated in local units and placed the same way via
`addPrimitive`. Phase 4's only external dependency was TE Phase 9.6 (local-space
triangulation), which has landed. Its one open item is the on-device verification Phase 6
calls for (a placed primitive's stored vertices are in local units; `objectTransform` maps
them to the expected NDC extent) — device-bound, so it rides on an on-device consumer rather
than the pure-CPU suite. Phase 5 is the Kreate consumer and can begin now — note Finding D
applies to it directly (Kreate's 2D/UI scene is a pixel-space viewport).
