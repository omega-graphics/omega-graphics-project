# Triangulation with Texture Attachments — Implementation Plan

## Goal

Support triangulating primitives with texture attachments end-to-end: params can specify a 2D or 3D texture attachment, triangulation produces per-vertex UVs (and optionally normals), and the result is consumable as a **GEMesh** — a GPU-ready mesh (vertex buffer, optional index buffer, vertex layout, and texture bindings). Texture loading, mesh buffer creation, and optimization are handled by our own cross-platform code on all backends.

## Current State

- **TETriangulationParams** supports up to one attachment per params: `TypeColor`, `TypeTexture2D`, or `TypeTexture3D`. Texture attachments only carry dimensions (`width`/`height` or `width`/`height`/`depth`), not a `GETexture` reference.
- **TETriangulationResult::TEMesh** holds CPU-side polygons; each vertex has `GPoint3D pt` and optional **AttachmentData** (color, `FVec<2>` texture2Dcoord, `FVec<3>` texture3Dcoord).
- Only the **Rect** triangulation path in `_triangulatePriv` assigns per-vertex UVs for `TypeTexture2D`; other primitives (RoundedRect, Ellipsoid, Prism, Path2D, Pyramid, Cylinder, Cone, Path3D) do not fill texture coordinates when a texture attachment is present.
- There is **no GEMesh type** today. Callers (e.g. 2DTest) manually iterate `result.meshes` and `vertexPolygons`, write position + color/UV into a `GEBuffer` via `GEBufferWriter`, then bind that buffer and draw. GPU triangulation (Metal) produces a similar CPU-side result via readback.
- **TETriangulationResult** has `gpuVertexBuffer` and `gpuVertexCount` but they are not consistently populated from triangulation; no shared path builds a full GPU mesh from a result.

## Non-Goals

- Changing the existing triangulation API shape (e.g. `triangulateSync` / `triangulateOnGPU`) or removing CPU triangulation.
- Implementing texture attachments inside GPU triangulation kernels (Metal compute) in this plan; that can be a follow-up.
- Adding new primitive types; the plan only adds texture-attachment support to existing primitives and a GEMesh abstraction.

---

## Phase 1: Attachment and Result Model

### 1.1 Texture attachment descriptor

- Extend **TETriangulationParams::Attachment** so that texture attachments can optionally reference a texture:
  - For **TypeTexture2D**: add optional `SharedHandle<GETexture> texture = nullptr`. When non-null, the mesh can later bind this texture for rendering; dimensions can still come from `texture2DData` or from the texture descriptor.
  - For **TypeTexture3D**: add optional `SharedHandle<GETexture> texture = nullptr` and use existing `texture3DData` for dimensions when texture is null.
- Keep a single attachment per params (no array of attachments in this phase).
- **Files**: `gte/include/omegaGTE/TE.h`, `gte/src/TE.cpp` (factory helpers if needed).

### 1.2 Triangulation: assign UVs for all primitives

- In **OmegaTriangulationEngineContext::_triangulatePriv**, for every branch that currently handles **TypeColor** (RoundedRect, Ellipsoid, RectangularPrism, Path2D, Pyramid, Cylinder, Cone, GraphicsPath3D), add handling for **TypeTexture2D** and **TypeTexture3D**:
  - **TypeTexture2D**: assign meaningful **texture2Dcoord** per vertex (and leave color as default or unused). Define UV mapping per primitive (e.g. RoundedRect: derive from normalized position; Ellipsoid: spherical or planar projection; Prism: per-face quad UVs; Path2D/Path3D: stroke-based or path-length-based UVs; Pyramid/Cylinder/Cone: cylindrical or planar as appropriate).
  - **TypeTexture3D**: assign **texture3Dcoord** per vertex where applicable (e.g. 3D primitives use object-space or normalized coordinates).
- Rect already has Texture2D UVs; ensure they remain correct and that Rect can also carry an optional texture reference.
- **Files**: `gte/src/TE.cpp`.

### 1.3 Optional normals in AttachmentData

- Add optional **FVec<3> normal** to **TETriangulationResult::AttachmentData** for 3D primitives (Prism, Pyramid, Cylinder, Cone, Path3D). Triangulation paths that produce solid 3D geometry should fill this when an attachment is present, so that GEMesh can use it for lighting or other shaders.
- **Files**: `gte/include/omegaGTE/TE.h`, `gte/src/TE.cpp` (all 3D and path triangulation branches).

---

## Phase 2: GEMesh abstraction

### 2.1 GEMesh type and descriptor

- Introduce **GEMesh** (or equivalent name) in the public API as the runtime mesh representation:
  - **GEMeshDescriptor** (or similar): vertex layout description (which attributes are present: position, color, uv2, uv3, normal), stride, and optionally index type (none / 16 / 32).
  - **GEMesh** holds:
    - `SharedHandle<GEBuffer> vertexBuffer`
    - `SharedHandle<GEBuffer> indexBuffer` (optional)
    - Vertex count, index count (if used), stride, layout enum or descriptor
    - Optional **texture bindings** (e.g. slot index → `SharedHandle<GETexture>`) for base color, normal map, etc., so that render code can bind the same mesh with different textures or use the texture that was attached at triangulation time.
- GEMesh is backend-agnostic; backends may store additional native data (e.g. D3D12 vertex/index views) internally.
- **Files**: new `gte/include/omegaGTE/GEMesh.h` (or under existing GE.h / a new mesh header), and backend-specific implementation headers/sources as needed.

### 2.2 Building GEMesh from TETriangulationResult

- Add a factory or engine method that builds a **GEMesh** from a **TETriangulationResult** and a **vertex layout** (e.g. position + color, or position + uv2, or position + uv2 + normal):
  - Flatten `result.meshes` and `vertexPolygons` into a single vertex buffer (and optionally an index buffer) with the requested layout.
  - If the result was produced with a texture attachment that had a `GETexture` reference, store that texture in the GEMesh’s texture bindings (e.g. slot 0 = base color texture).
  - Buffer creation uses existing **OmegaGraphicsEngine::makeBuffer** (and optionally makeHeap); no new buffer API required.
- **Files**: `gte/include/omegaGTE/GEMesh.h`, `gte/src/common/GEMesh.cpp` (or backend-neutral builder).

### 2.3 Command encoding: draw GEMesh

- Extend **GECommandQueue** (or the command encoder used for draw calls) so that the caller can:
  - Set vertex buffer (and optional index buffer) from a **GEMesh**.
  - Bind textures (and samplers) from the GEMesh’s bindings for the current pass.
- This may be a thin wrapper over existing `setVertexBuffer` / `bindResourceAtVertexShader` (or fragment) plus a draw call with vertex/index count from the mesh.
- **Files**: `gte/include/omegaGTE/GECommandQueue.h`, `gte/include/omegaGTE/GERenderTarget.h`, and backend implementations (Metal, D3D12, Vulkan).

---

## Phase 3: Testing and documentation

- Add or extend tests that:
  - Triangulate a primitive (e.g. Rect, RoundedRect, Prism) with **TypeTexture2D** (and optionally a **GETexture** reference).
  - Build a **GEMesh** from the result with a layout that includes UVs.
  - Render the mesh with the bound texture (and optionally verify visually or via readback).
- Prefer reusing existing 2DTest-style apps: add a code path that uses texture attachment and GEMesh instead of manual vertex writing.
- Document in `gte/docs` or in-code:
  - How to add a texture attachment to triangulation params.
  - How to build a GEMesh from a TETriangulationResult and bind its textures.

---

## Implementation order (suggested)

1. **Phase 1.1–1.2**: Attachment descriptor (optional texture handle) and UV assignment for all primitives in `_triangulatePriv`.
2. **Phase 1.3**: Optional normals in AttachmentData and fill for 3D primitives.
3. **Phase 2.1–2.2**: GEMesh type, descriptor, and builder from TETriangulationResult (backend-neutral buffer creation).
4. **Phase 2.3**: Command encoding support to draw a GEMesh and bind its textures.
5. **Phase 3**: Tests and documentation.

---

## Summary

| Area | Change |
|------|--------|
| **Triangulation params** | Texture attachments can carry optional `SharedHandle<GETexture>`; dimensions remain in existing union. |
| **Triangulation result** | All primitives emit texture2D/texture3D coords (and optional normals) when a texture attachment is set. |
| **GEMesh** | New type: vertex (+ optional index) buffer, layout, and texture bindings; buildable from TETriangulationResult. |
| **Command encoding** | Draw GEMesh (set vertex/index buffer, bind mesh textures). |
