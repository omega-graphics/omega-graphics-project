# Mesh Parser Extension — Implementation Plan

## Goal

Grow the shared, backend-neutral `MeshParser` (`gte/src/common/MeshParser.{h,cpp}`)
beyond its v1 contract — a flat, non-indexed triangle stream with one base-color
texture — into a loader that can emit indexed geometry, richer material/texture
sets, multi-material submeshes, and tangents, applied **uniformly across OBJ /
glTF / FBX** wherever each format can supply the data. A secondary aim, called
out explicitly below, is to identify which of these are *cheap* to add on the
existing OBJ (inline) and glTF (`cgltf`) paths so we get the most coverage per
change.

This plan is the follow-up track to the FBX (`ufbx`) work landed in
`GEMesh-TextureAssets-Implementation-Plan.md` §3.4, and reuses that plan's
vocabulary (`GEMesh`, `GEMeshDescriptor`, `GEMeshAsset::LoadOptions`).

## Current State (v1 — shipped)

- **Shared `MeshParser::parseMesh`** dispatches by extension and produces a flat
  **non-indexed** triangle stream (`desc.indexType` must be `None`). Layout is
  fixed: Position → UV2 → UV3 → Normal → Color; missing requested attributes are
  zero-filled; only the **first** base-color texture path is surfaced
  (`ParsedMesh::baseColorTexturePath`). Vertices are emitted in the file's raw
  coordinate space / units (no axis or unit conversion) on every format.
- **OBJ** (inline): `v` / `vt` / `vn` / `f` / `mtllib` / `usemtl` / `map_Kd`;
  fan-triangulates n-gons; negative indices; multi-material collapses into one
  mesh.
- **glTF** (`cgltf`): triangle primitives only (others skipped); reads
  `POSITION` / `TEXCOORD_0` / `NORMAL` / `COLOR_0`; base color from
  `pbr_metallic_roughness`; node/instance transforms ignored (geometry emitted
  in mesh-local space); the existing index accessor is **flattened**, not passed
  through.
- **FBX** (`ufbx`): per-face triangulation via `ufbx_triangulate_face`; base
  color via `pbr.base_color` → `fbx.diffuse_color` fallback; skinning,
  animation, tangents, and multi-material splitting dropped.
- **Backends**: D3D12 / Vulkan consume the shared parser for all formats; Metal
  uses Model I/O for OBJ/glTF and routes only `.fbx` to the shared parser.
- **`GEMesh`** (`gte/include/omegaGTE/GEMesh.h`) already carries `indexBuffer` /
  `indexCount` (unused by the parser today) and a `textureBindings`
  `map<unsigned slot, GETexture>` — but has **no submesh ranges**. The vertex
  attribute mask has Position / UV2 / UV3 / Normal / Color — **no tangent**.

## Non-Goals (this plan)

- **Skinning / animation / morph targets.** Joints, weights, skeletons, and
  animation clips are a much larger, separate track (new GEMesh skeletal data,
  a runtime pose/skin path). Both `cgltf` and `ufbx` expose this, but it does
  not belong here. Flagged as a future track only.
- **Compressed glTF** (`KHR_draco_mesh_compression`, `EXT_meshopt_compression`):
  needs an extra decoder dependency (draco / meshoptimizer). Out of scope unless
  a concrete asset demands it.
- **Authoring / writing** meshes — load path only.
- Reworking the triangulation-engine → GEMesh builder (`buildMeshFromTriangulation`)
  except where a shared GEMesh field (submeshes, tangent attr) is added that it
  must also populate.

---

## Phase A: Indexed output (shared)

The single biggest memory / bandwidth win, and format-agnostic.

- Lift the `desc.indexType != None` rejection in `parseMesh`. When the caller
  requests `UInt16` / `UInt32`, deduplicate the packed per-vertex records (hash
  the `stride`-byte vertex blob → first-seen index) and emit a separate index
  stream; `ParsedMesh` grows an `std::vector<uint32_t> indices` (plus the
  existing `packed` now holding only unique vertices).
- Promote the result into `GEMesh::indexBuffer` / `indexCount` (already present)
  in every asset backend's build step; warn (and fall back to `UInt32`, or
  refuse) when unique-vertex count exceeds 65535 under `UInt16`.
- **Per-format fast paths** (the cheap part):
  - **glTF**: when a primitive already has an index accessor, pass those indices
    through (remapped per-primitive into the merged buffer) instead of
    re-deduplicating — exact and faster. `cgltf_accessor_read_index` is already
    used; we just stop flattening.
  - **OBJ**: the `v/vt/vn` triple already *is* a vertex key — map each distinct
    triple to a unique index instead of appending a fresh vertex per corner.
  - **FBX**: dedup the triangulated corner stream (ufbx corner indices are not
    directly reusable as GEMesh indices because attributes are per-corner).
- Draw path: confirm `GECommandQueue` / the draw encoder binds an index buffer
  and issues an indexed draw when `indexCount > 0` (added in Phase 2.3 of the
  parent plan — **verify and wire if missing**).
- **Files**: `MeshParser.{h,cpp}`; `GEMetalMeshAsset.mm`,
  `GED3D12MeshAsset.cpp`, `GEVulkanMeshAsset.cpp` (build step); possibly
  `GECommandQueue.h` + backends for the indexed draw.

## Phase B: Material maps beyond base color (shared + per-format)

- Define standard texture-role slots alongside the existing `baseColorSlot`
  (e.g. a small enum or `LoadOptions { unsigned baseColorSlot, normalSlot,
  metallicRoughnessSlot, emissiveSlot, occlusionSlot; }`). `ParsedMesh` grows a
  role→path table (replacing the single `baseColorTexturePath`, or alongside it
  for back-compat).
- **glTF** (`cgltf`) — **cheap**, all directly available:
  `normal_texture`, `pbr_metallic_roughness.metallic_roughness_texture`,
  `emissive_texture`, `occlusion_texture`.
- **FBX** (`ufbx`) — available via material maps: `pbr.normal_map`,
  `pbr.metalness` / `pbr.roughness`, `pbr.emission_color`.
- **OBJ** (`.mtl`) — moderate: `map_Bump` / `bump` (normal/height), `map_Ks`,
  `map_Ke`, and the PBR extension maps `map_Pr` / `map_Pm`. Parsing mirrors the
  existing `map_Kd` handling (last-token path, option flags stripped).
- Asset backends bind each present role into `GEMesh::textureBindings` at its
  slot and create a `TextureAsset` per map.
- **Files**: `GEMeshAsset.h` (slots), `MeshParser.{h,cpp}`, all three asset
  backends.

## Phase C: Multi-material submeshes

- Add a submesh table to `GEMesh`: `std::vector<Submesh>` where
  `Submesh { unsigned indexOffset/vertexOffset; unsigned count; std::map<unsigned,SharedHandle<GETexture>> textureBindings; }`
  (or a material index into a shared bindings list). Single-material meshes
  remain one submesh, so existing callers are unaffected.
- `MeshParser` splits by material instead of collapsing:
  - **glTF**: each primitive already maps to one material — natural 1:1.
  - **OBJ**: split on `usemtl` runs.
  - **FBX**: `mesh->face_material` indexes `mesh->materials` — group faces by
    material id.
- Draw path: iterate submeshes, bind that submesh's textures, draw its range.
- This is the largest GEMesh-surface change here (new draw-time iteration), so
  it sequences after A/B.
- **Files**: `GEMesh.h`, `MeshParser.{h,cpp}`, asset backends, draw encoder.

## Phase D: Tangents

- Add `GEMeshAttrTangent` (float4 — xyz tangent + w handedness) to
  `GEMeshVertexAttribute` and `geMeshStrideFor`; fix the documented attribute
  order (insert after Normal).
- **glTF**: read the `TANGENT` attribute (`cgltf_attribute_type_tangent`) when
  present; otherwise generate.
- **FBX**: `mesh->vertex_tangent` / `vertex_bitangent` when present; otherwise
  generate.
- **OBJ**: always generate (no tangent in the format).
- Generation: per-triangle tangent from positions + UV + normal (a small
  MikkTSpace-style accumulate-and-orthonormalize pass; shared helper so all
  three formats use it).
- **Files**: `GEMesh.h`, `MeshParser.{h,cpp}` (+ shared tangent-gen helper),
  shader-side layout note in `API.rst`.

## Phase E: Optional coordinate / unit normalization

The opt-in counterpart to the **raw** default we locked in for FBX.

- Add `LoadOptions::normalizeToEngineSpace` (default **off** → today's raw
  behavior on every format).
- When on:
  - **FBX** (`ufbx`): set `ufbx_load_opts.target_axes` (right-handed Y-up),
    `target_unit_meters = 1.0`, and `space_conversion` so Z-up / centimeter
    exports from Maya / 3ds Max / Blender land consistently.
  - **glTF**: already Y-up / meters by spec — no-op (optionally validate).
  - **OBJ**: no embedded axis/unit metadata — no-op (documented).
- **Files**: `GEMeshAsset.h`, `MeshParser.{h,cpp}` (`parseFbx`).

## Phase F: "Can we extend OBJ / cgltf?" — investigation outcome

Concrete feasibility for the two existing paths, so we know what's worth doing.

### OBJ (inline parser) — feasible / cheap
- **Smoothing groups (`s`)** → when `vn` is absent, generate area-weighted
  vertex normals instead of zero-filling. Higher value than it looks: many OBJ
  exports omit normals.
- **Per-vertex color** (`v x y z r g b`, a common extension) → fill Color
  instead of default white.
- **Extra `.mtl` maps** for Phase B (`map_Bump`, `map_Ks`, `map_Ke`,
  `map_Pr`/`map_Pm`) — same mechanism as `map_Kd`.
- Already handled, no work: negative indices, n-gon fans, `mtllib` / `usemtl`.

### glTF (`cgltf`) — feasible / cheap
- **Index pass-through** (Phase A) — `cgltf` hands us the index accessor
  directly; cheapest path to indexed output of the three formats.
- **Second UV set / vertex colors** — `TEXCOORD_1`, `COLOR_n` are just more
  attributes (`findAttr` already takes a type; add index-aware lookup).
- **Node-transform baking** — walk the `cgltf_node` hierarchy, compose
  world matrices, and transform positions/normals so multi-node scenes assemble
  correctly. Today everything is emitted in mesh-local space; this fixes models
  that look "exploded." Moderate but high-value.
- **All Phase-B material textures** — present on `cgltf_material` directly.

### Needs a new capability / dependency (defer)
- **`.glb` embedded images**: `cgltf` exposes `image->buffer_view`, but
  `TextureAsset` is **path-based** today — needs an in-memory `TextureAsset`
  load entry point (cross-reference the TextureAsset track) before glTF embedded
  textures can be bound.
- **Draco / meshopt** compressed primitives: extra decoder dependency — out of
  scope (see Non-Goals).

## Phase G: Testing & documentation

- Extend `AssetTest` (Metal) and the D3D12 / Vulkan equivalents:
  - Indexed load — assert `indexCount` / `vertexCount` and that dedup shrank the
    vertex count vs. the flat stream.
  - A normal-mapped, multi-UV glTF — assert the extra texture slots are bound.
  - A multi-material OBJ — assert submesh count.
  - Tangent presence when `GEMeshAttrTangent` is requested.
  - `normalizeToEngineSpace` on a Z-up FBX — assert axis swap.
- `cgltf` and `ufbx` both ship sample assets (`gte/deps/ufbx/data/…`) we can
  point fixtures at, as the FBX cube case already does.
- Document new `LoadOptions` slots, the tangent attribute, submesh iteration,
  and the normalization flag in `gte/docs/API.rst` and this plan's status.

---

## Suggested order

1. **Phase A — indexed output** (biggest perf win, shared, with cheap glTF/OBJ
   fast paths).
2. **Phase B — material maps** (cheap on glTF/FBX, moderate on OBJ).
3. **Phase C — multi-material submeshes** (largest GEMesh-surface change).
4. **Phase D — tangents** (depends on the attribute-order change; pairs with
   normal maps from B).
5. **Phase E — coordinate/unit normalization** (small, opt-in).
6. **Phase F niceties** — fold the cheap OBJ/glTF items in alongside the phase
   that needs them (smoothing-group normals with B, node-baking with A).

## Summary table

| Extension | OBJ | glTF (cgltf) | FBX (ufbx) | GEMesh / API change |
|-----------|-----|--------------|------------|---------------------|
| **Indexed output** | dedup `v/vt/vn` triples | pass through index accessor | dedup corner stream | uses existing `indexBuffer`/`indexCount`; indexed draw |
| **Material maps** (normal / MR / emissive / occlusion) | `.mtl` `map_Bump`/`map_Ks`/`map_Ke`/`map_Pr`/`map_Pm` | `normal`/`metallic_roughness`/`emissive`/`occlusion` textures | `pbr.normal_map` / `metalness` / `roughness` / `emission_color` | new `LoadOptions` slots |
| **Multi-material submeshes** | split on `usemtl` | per-primitive material | `face_material` grouping | new `GEMesh` submesh ranges + draw iteration |
| **Tangents** | generate | `TANGENT` or generate | `vertex_tangent` or generate | new `GEMeshAttrTangent` (float4) |
| **Coord/unit normalize** | n/a (no metadata) | n/a (spec Y-up/m) | `ufbx` `space_conversion` | `LoadOptions::normalizeToEngineSpace` (default off) |
| **Node-transform baking** | n/a | walk `cgltf_node` tree | (instances) | none (correctness) |
| **`.glb` embedded textures** | n/a | `image->buffer_view` | embedded textures | needs in-memory `TextureAsset` load |
| **Skinning / animation** | n/a | available | available | **future track — not in this plan** |
