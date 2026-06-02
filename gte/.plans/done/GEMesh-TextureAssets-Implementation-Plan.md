# GEMesh and Texture Assets — Implementation Plan

## Goal

Support triangulating primitives with texture attachments end-to-end: params can specify a 2D or 3D texture attachment, triangulation produces per-vertex UVs (and optionally normals), and the result is consumable as a **GEMesh** — a GPU-ready mesh (vertex buffer, optional index buffer, vertex layout, and texture bindings).

In addition, provide a **TextureAsset** class for high-level texture loading (from common image and container formats) and a **MeshAsset** class for loading meshes from standard 3D model formats. These asset classes use platform-native libraries for loading and GPU upload, abstracting away format details and backend differences.

## Current State

- **TETriangulationParams** supports up to one attachment per params: `TypeColor`, `TypeTexture2D`, or `TypeTexture3D`. Texture attachments only carry dimensions (`width`/`height` or `width`/`height`/`depth`), not a `GETexture` reference.
- **TETriangulationResult::TEMesh** holds CPU-side polygons; each vertex has `GPoint3D pt` and optional **AttachmentData** (color, `FVec<2>` texture2Dcoord, `FVec<3>` texture3Dcoord).
- Only the **Rect** triangulation path in `_triangulatePriv` assigns per-vertex UVs for `TypeTexture2D`; other primitives (RoundedRect, Ellipsoid, Prism, Path2D, Pyramid, Cylinder, Cone, Path3D) do not fill texture coordinates when a texture attachment is present.
- There is **no GEMesh type** today. Callers (e.g. 2DTest) manually iterate `result.meshes` and `vertexPolygons`, write position + color/UV into a `GEBuffer` via `GEBufferWriter`, then bind that buffer and draw. GPU triangulation (Metal) produces a similar CPU-side result via readback.
- **TETriangulationResult** has `gpuVertexBuffer` and `gpuVertexCount` but they are not consistently populated from triangulation; no shared path builds a full GPU mesh from a result.

## Non-Goals

- Changing the existing triangulation API shape (e.g. `triangulateSync` / `triangulateOnGPU`) or removing CPU triangulation.
- Implementing texture attachments inside GPU triangulation kernels (Metal compute) in this plan; that can be a follow-up.
- Adding new primitive types; the plan only adds texture-attachment support to existing primitives, a GEMesh abstraction, and asset loading.
- Writing a custom image decoder or mesh parser from scratch when a platform-native library already handles the format.

---

## Phase 1: Attachment and Result Model ✅

### 1.1 Texture attachment descriptor ✅

- Extend **TETriangulationParams::Attachment** so that texture attachments can optionally reference a texture:
  - For **TypeTexture2D**: add optional `SharedHandle<GETexture> texture = nullptr`. When non-null, the mesh can later bind this texture for rendering; dimensions can still come from `texture2DData` or from the texture descriptor.
  - For **TypeTexture3D**: add optional `SharedHandle<GETexture> texture = nullptr` and use existing `texture3DData` for dimensions when texture is null.
- Keep a single attachment per params (no array of attachments in this phase).
- **Files**: `gte/include/omegaGTE/TE.h`, `gte/src/TE.cpp` (factory helpers if needed).

### 1.2 Triangulation: assign UVs for all primitives ✅

- In **OmegaTriangulationEngineContext::_triangulatePriv**, for every branch that currently handles **TypeColor** (RoundedRect, Ellipsoid, RectangularPrism, Path2D, Pyramid, Cylinder, Cone, GraphicsPath3D), add handling for **TypeTexture2D** and **TypeTexture3D**:
  - **TypeTexture2D**: assign meaningful **texture2Dcoord** per vertex (and leave color as default or unused). Define UV mapping per primitive (e.g. RoundedRect: derive from normalized position; Ellipsoid: spherical or planar projection; Prism: per-face quad UVs; Path2D/Path3D: stroke-based or path-length-based UVs; Pyramid/Cylinder/Cone: cylindrical or planar as appropriate).
  - **TypeTexture3D**: assign **texture3Dcoord** per vertex where applicable (e.g. 3D primitives use object-space or normalized coordinates).
- Rect already has Texture2D UVs; ensure they remain correct and that Rect can also carry an optional texture reference.
- **Files**: `gte/src/TE.cpp`.

### 1.3 Optional normals in AttachmentData ✅

- Add optional **FVec<3> normal** to **TETriangulationResult::AttachmentData** for 3D primitives (Prism, Pyramid, Cylinder, Cone, Path3D). Triangulation paths that produce solid 3D geometry should fill this when an attachment is present, so that GEMesh can use it for lighting or other shaders.
- **Files**: `gte/include/omegaGTE/TE.h`, `gte/src/TE.cpp` (all 3D and path triangulation branches).

---

## Phase 2: GEMesh abstraction ✅

### 2.1 GEMesh type and descriptor ✅

- Introduce **GEMesh** (or equivalent name) in the public API as the runtime mesh representation:
  - **GEMeshDescriptor** (or similar): vertex layout description (which attributes are present: position, color, uv2, uv3, normal), stride, and optionally index type (none / 16 / 32).
  - **GEMesh** holds:
    - `SharedHandle<GEBuffer> vertexBuffer`
    - `SharedHandle<GEBuffer> indexBuffer` (optional)
    - Vertex count, index count (if used), stride, layout enum or descriptor
    - Optional **texture bindings** (e.g. slot index → `SharedHandle<GETexture>`) for base color, normal map, etc., so that render code can bind the same mesh with different textures or use the texture that was attached at triangulation time.
- GEMesh is backend-agnostic; backends may store additional native data (e.g. D3D12 vertex/index views) internally.
- **Files**: new `gte/include/omegaGTE/GEMesh.h` (or under existing GE.h / a new mesh header), and backend-specific implementation headers/sources as needed.

### 2.2 Building GEMesh from TETriangulationResult ✅

- Add a factory or engine method that builds a **GEMesh** from a **TETriangulationResult** and a **vertex layout** (e.g. position + color, or position + uv2, or position + uv2 + normal):
  - Flatten `result.meshes` and `vertexPolygons` into a single vertex buffer (and optionally an index buffer) with the requested layout.
  - If the result was produced with a texture attachment that had a `GETexture` reference, store that texture in the GEMesh’s texture bindings (e.g. slot 0 = base color texture).
  - Buffer creation uses existing **OmegaGraphicsEngine::makeBuffer** (and optionally makeHeap); no new buffer API required.
- **Files**: `gte/include/omegaGTE/GEMesh.h`, `gte/src/common/GEMesh.cpp` (or backend-neutral builder).

### 2.3 Command encoding: draw GEMesh ✅

- Extend **GECommandQueue** (or the command encoder used for draw calls) so that the caller can:
  - Set vertex buffer (and optional index buffer) from a **GEMesh**.
  - Bind textures (and samplers) from the GEMesh’s bindings for the current pass.
- This may be a thin wrapper over existing `setVertexBuffer` / `bindResourceAtVertexShader` (or fragment) plus a draw call with vertex/index count from the mesh.
- **Files**: `gte/include/omegaGTE/GECommandQueue.h`, `gte/include/omegaGTE/GERenderTarget.h`, and backend implementations (Metal, D3D12, Vulkan).

---

## Phase 3: Asset Loading (TextureAsset and MeshAsset) — Apple/Metal first

Phase 3 lands the public asset API and a working **Metal** implementation on
macOS/iOS. The Windows (D3D12) and Linux/Android (Vulkan) implementations are
deferred to Phase 3.4 so the API can be exercised on the platform that is
currently buildable, without writing two backends blind. The headers expose
the full API; non-Metal builds compile a stub that throws "not implemented"
until 3.4 lands.

### 3.1 TextureAsset class — Metal impl

Introduce a **TextureAsset** class that handles loading textures from common
image and container formats and uploading them into a `GETexture`. The public
API is backend-agnostic; the implementation delegates to a platform-native
library for decoding, mip generation, and format conversion.

```
TextureAsset
├── load(path)          → decode image, create GETexture, upload
├── loadAsync(path)     → non-blocking variant (std::future)
├── texture()           → SharedHandle<GETexture>
├── descriptor()        → TextureDescriptor (dimensions, format, mips)
└── release()           → free GPU + CPU resources
```

**Metal backend (this phase):** **MetalKit** (`MTKTextureLoader`) loads PNG,
JPEG, TIFF, BMP, HDR, KTX, PVR, and ASTC and returns an `MTLTexture` directly.
The Metal `TextureAsset` wraps that texture in a `GEMetalTexture` so the rest
of the engine sees a normal `SharedHandle<GETexture>`. Mip generation and
sRGB handling come from `MTKTextureLoader` options.

**Files (this phase):**
- new `gte/include/omegaGTE/GETextureAsset.h`
- new `gte/src/metal/GEMetalTextureAsset.mm`
- new stubs `gte/src/d3d12/GED3D12TextureAsset.cpp` and
  `gte/src/vulkan/GEVulkanTextureAsset.cpp` that throw on `load` until 3.4.

### 3.2 MeshAsset class — Metal impl

Introduce a **MeshAsset** class that loads mesh data from standard 3D model
formats and produces a **GEMesh**. Like TextureAsset, the public API is
backend-agnostic.

```
MeshAsset
├── load(path)          → decode mesh, create GEMesh (vertex + index buffers)
├── loadAsync(path)     → non-blocking variant (std::future)
├── mesh()              → SharedHandle<GEMesh>
├── textureAssets()     → vector<SharedHandle<TextureAsset>> for embedded/referenced textures
└── release()           → free GPU + CPU resources
```

**Metal backend (this phase):** **Model I/O** (`MDLAsset`) plus **MetalKit**
(`MTKMesh`) loads **OBJ**, **glTF 2.0**, USD, and Alembic out of the box and
produces Metal vertex/index buffers directly. The Metal `MeshAsset` walks
the loaded `MDLAsset`, builds a `GEMesh` whose vertex buffer follows the
project's `GEMeshDescriptor` attribute order (Position → UV2 → Normal → Color),
and resolves any material base-color textures to per-mesh `TextureAsset`s.

**Files (this phase):**
- new `gte/include/omegaGTE/GEMeshAsset.h`
- new `gte/src/metal/GEMetalMeshAsset.mm`
- new stubs `gte/src/d3d12/GED3D12MeshAsset.cpp` and
  `gte/src/vulkan/GEVulkanMeshAsset.cpp` that throw on `load` until 3.4.

### 3.3 Asset integration with GEMesh and triangulation

- **GEMesh from MeshAsset**: `MeshAsset::mesh()` returns a fully populated
  `GEMesh` with vertex/index buffers and layout — the same type produced by
  the triangulation builder in Phase 2.2. Rendering code does not distinguish
  between a triangulated mesh and a loaded mesh.
- **TextureAsset in GEMesh bindings**: When a MeshAsset references textures
  (e.g. a glTF material's base color texture), the loader creates
  `TextureAsset` instances and stores their `SharedHandle<GETexture>` in
  `GEMesh::textureBindings` keyed by the project-standard slot ids
  (slot 0 = base color in this phase). A `TextureAsset` loaded standalone
  can be attached to a triangulated `GEMesh` by writing into
  `mesh->textureBindings` directly.
- **Lifetime**: `SharedHandle<GETexture>` keeps the texture alive as long as
  any mesh or user code holds a reference. `release()` is provided as an
  explicit early-free hook but is not required — destruction handles it.

### 3.4 D3D12 ✅ — Vulkan ✅

| Platform | Library | Status | Notes |
|----------|---------|--------|-------|
| **Windows (D3D12)** | **DirectXTex** + **DirectXMesh** + **cgltf** + inline OBJ | **Done** | DirectXTex loads DDS / HDR / TGA / WIC formats (PNG/JPEG/TIFF/BMP), generates mipmaps, handles BC compression and sRGB promotion. DirectXMesh is linked but unused in v1 (reserved for tangent / cache-optimization passes). `cgltf` parses glTF 2.0; OBJ is parsed by an inline ~120-line parser in `MeshParser.cpp`. |
| **Linux / Android (Vulkan)** | **OmegaCommon::Img** (libpng / turbojpeg / libtiff) + shared `MeshParser` (cgltf + inline OBJ) | **Done** | Image decoding goes through the in-tree `OmegaCommon::Img` codec stack already linked transitively via OmegaCommon — no `libktx` / `stb_image` fetch was needed for the v1 surface. Decoded PNG/JPEG/TIFF is normalized to 8-bit RGBA and uploaded into a `ToGPU` (LINEAR-tiled, host-visible) `GETexture` via `copyBytes`. Mesh loading reuses the shared `MeshParser` exactly like the D3D12 backend; the vertex buffer is a `BufferDescriptor::Upload` allocation mapped through VMA. |

For Vulkan (or any of the other platforms that can't support specifc image type), we will use our own image codec from OmegaCommon. (We can add `stb_image` to our image codec for extra format support.)
Same goes with our mesh_parser. It should also be accessible from macOS so we can parse other meshes that are normally supported on there. (fbx)

*NOTES (post-implementation):*



- The mesh parser **is** shared across platforms — `gte/src/common/MeshParser.{h,cpp}` is backend-neutral and ready for Vulkan to consume directly. Metal stays on Model I/O / `MTKMesh` (its own path) because the platform library is strictly more capable.
- **OBJ**: implemented inline in `MeshParser.cpp` (handles `v` / `vt` / `vn` / `f` / `mtllib` / `usemtl` / `map_Kd`, fan-triangulates n-gons, supports negative indices). The original plan called for `ssell/OBJParser`; we dropped the dependency to keep the build graph small and avoid a third-party API whose surface didn't pay rent for ~120 lines of trivial parsing.
- **FBX (`ufbx`)** ✅ implemented in the shared `MeshParser` (`parseFbx`). `ufbx`
  loads both binary and ASCII variants; faces are triangulated per-face via
  `ufbx_triangulate_face`, and legacy (non-PBR) FBX materials are read through
  ufbx's `pbr.base_color` view (falling back to `fbx.diffuse_color`) so the
  first base-color texture resolves the same way as glTF/OBJ. Vertices are
  emitted in the file's **raw** coordinate space / units — no axis or unit
  conversion — matching the glTF/OBJ paths (default `ufbx_load_opts`). Skinning,
  animation, tangents, and multi-material splitting are still dropped (same v1
  scope as the other formats). Because FBX is the one common format Model I/O
  cannot load, the **Metal** asset loader now routes `.fbx` through the shared
  parser too (OBJ/glTF stay on Model I/O); `MeshParser.cpp` is consequently
  compiled on every backend now, and `cgltf` + `ufbx` were added to the macOS
  dep set. Verified end-to-end on Metal by `AssetTest` (loads a ufbx bundled
  cube → 12 triangles / 36 vertices). The dropped scope (skinning, animation,
  tangents, multi-material submeshes) and broader parser extensions — including
  what's cheap to add on the OBJ / `cgltf` paths — are tracked in
  `../Mesh-Parser-Extension-Plan.md`.
- **BC / HDR pixel formats**: `TexturePixelFormat` only models 5 formats today. `GED3D12TextureAsset` reports a best-effort `descriptor()` (warns once on unmapped DXGI formats) and constructs the SRV directly from the source DXGI format, so BC1–BC7 / HDR / 16F textures bind correctly even though `descriptor().pixelFormat` is lossy. Extending the engine-level enum is future work.
- **Texture upload**: a transient `D3D12_COMMAND_LIST_TYPE_DIRECT` queue + fence is created per `load()`. Avoids interleaving with the engine's main render queue and keeps the loader self-contained at the cost of a queue allocation per file. Acceptable for asset-load workloads.

*Vulkan-specific post-implementation notes:*

- **Image codec choice**: per the directive in the original notes ("we will use our own image codec from OmegaCommon"), the Vulkan backend skips the `libktx` / `stb_image` fetch and decodes through `OmegaCommon::Img::loadFromFile`. PNG / JPEG / TIFF are the v1 surface. Folding `stb_image` into `OmegaCommon::Img` to cover BMP / HDR / KTX is a clean follow-up — `GEVulkanTextureAsset` would not need to change.

The real question: should KTX loading be a part of OmegaCommon::Img or TextureAsset??

- **Pixel format normalization**: the decoded `BitmapImage` is forced to 8-bit RGBA before upload. RGB sources get an alpha pad of `0xFF`; 16-bit PNGs are already stripped to 8-bit by the codec. `RGBA8Unorm_SRGB` vs `RGBA8Unorm` follows `LoadOptions::sRGB`.
- **Mip generation**: `generateMipmaps` is honored as a best-effort hint only — runtime mip generation on Vulkan requires a graphics queue + `vkCmdBlitImage` chain, which lives in the upcoming upload-queue helper. v1 uploads mip 0 and logs a one-shot notice. The D3D12 path's transient-queue trick doesn't translate directly because Vulkan needs the chain to live on a real graphics queue for SRGB blit support.
- **Texture upload**: `GETexture::ToGPU` allocates a `LINEAR`-tiled, `HOST_VISIBLE` `VkImage`, so `copyBytes` does a direct memcpy through `vmaMapMemory`. No staging buffer, no transfer-queue plumbing — same as how `getBytes` works in reverse. Compressed and HDR formats would need the staging path; not in scope here.
- **Mesh vertex buffer**: `engine->makeBuffer({ Upload, ... })` returns a `CPU_TO_GPU` VMA buffer that the loader maps once and memcpys into. Mirrors the D3D12 backend exactly.

**Files (this phase):**
- replaced `gte/src/d3d12/GED3D12TextureAsset.cpp` with a DirectXTex-backed implementation
- replaced `gte/src/d3d12/GED3D12MeshAsset.cpp` with a `MeshParser`-driven implementation
- replaced `gte/src/vulkan/GEVulkanTextureAsset.cpp` with an `OmegaCommon::Img`-backed implementation
- replaced `gte/src/vulkan/GEVulkanMeshAsset.cpp` with a `MeshParser`-driven implementation
- new `gte/src/common/MeshParser.h` / `gte/src/common/MeshParser.cpp` (shared, backend-neutral)
- `gte/AUTOMDEPS`: `cgltf` now declared for `windows`, `linux`, and `android` platforms (Vulkan picks it up via the shared parser)
- `gte/CMakeLists.txt`: under `TARGET_DIRECTX`, `add_subdirectory` + link `DirectXTex` + `DirectXMesh`, include `cgltf`. Under `TARGET_VULKAN`, also includes `cgltf` so `MeshParser.cpp` compiles. No extra link entries needed for image decoding — `libpng` / `turbojpeg` / `libtiff` are already pulled in transitively via OmegaCommon.

---

## Phase 4: Testing and documentation

- Add or extend tests that:
  - Triangulate a primitive (e.g. Rect, RoundedRect, Prism) with **TypeTexture2D** (and optionally a **GETexture** reference).
  - Build a **GEMesh** from the result with a layout that includes UVs.
  - Render the mesh with the bound texture (and optionally verify visually or via readback).
  - Load a texture from disk via **TextureAsset** (e.g. a PNG on each backend) and bind it to a triangulated GEMesh.
  - Load a mesh from disk via **MeshAsset** (e.g. a glTF or OBJ file) and render it with its associated textures.
- Prefer reusing existing 2DTest-style apps: add a code path that uses texture attachment and GEMesh instead of manual vertex writing.
- Document in `gte/docs` or in-code:
  - How to add a texture attachment to triangulation params.
  - How to build a GEMesh from a TETriangulationResult and bind its textures.
  - How to load textures and meshes via the asset classes on each platform.

---

## Implementation order (suggested)

1. **Phase 1.1–1.2**: Attachment descriptor (optional texture handle) and UV assignment for all primitives in `_triangulatePriv`.
2. **Phase 1.3**: Optional normals in AttachmentData and fill for 3D primitives.
3. **Phase 2.1–2.2**: GEMesh type, descriptor, and builder from TETriangulationResult (backend-neutral buffer creation).
4. **Phase 2.3**: Command encoding support to draw a GEMesh and bind its textures.
5. **Phase 3.1**: TextureAsset — Metal (MetalKit) impl + stubs on D3D12/Vulkan.
6. **Phase 3.2**: MeshAsset — Metal (Model I/O / MTKMesh) impl + stubs on D3D12/Vulkan.
7. **Phase 3.3**: Wire MeshAsset outputs into GEMesh bindings.
8. **Phase 3.4 (D3D12 ✅, Vulkan ✅)**: D3D12 (DirectXTex / DirectXMesh + cgltf + inline OBJ) and Vulkan (`OmegaCommon::Img` + shared `MeshParser`) are both complete.
9. **Phase 4**: Tests and documentation.

---

## Summary

| Area | Change |
|------|--------|
| **Triangulation params** | Texture attachments can carry optional `SharedHandle<GETexture>`; dimensions remain in existing union. |
| **Triangulation result** | All primitives emit texture2D/texture3D coords (and optional normals) when a texture attachment is set. |
| **GEMesh** | New type: vertex (+ optional index) buffer, layout, and texture bindings; buildable from TETriangulationResult or MeshAsset. |
| **TextureAsset** | High-level texture loading via DirectXTex (D3D12 ✅), MetalKit (Metal ✅), `OmegaCommon::Img` (Vulkan ✅). |
| **MeshAsset** | High-level mesh loading via shared `MeshParser` (cgltf + inline OBJ + `ufbx` FBX) on D3D12 ✅ and Vulkan ✅; MetalKit / Model I/O on Metal ✅, with `.fbx` routed to the shared parser (Model I/O can't load FBX). |
| **Command encoding** | Draw GEMesh (set vertex/index buffer, bind mesh textures). |
