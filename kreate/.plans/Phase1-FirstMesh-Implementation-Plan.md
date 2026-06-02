# kREATE Phase 1 — First Mesh on Screen — Implementation Plan

## Context

`Engine-Roadmap.md` Phase 1 turns the commented-out draw loop in
`Scene::render` into a real one: a single hand-built mesh, transformed
per-frame, drawn through a user-supplied OmegaSL pipeline.

Phase 0 (window + clear + present) is done. The pipeline subsystem is in
place (`Pipeline` + `PipelineFactory`, runtime / pre-compiled paths).
`Object` exists as transform + pipeline; `Scene` walks the hierarchy but
the draw loop is commented out, waiting on a mesh handle.

GTE already gives us `GEMesh` (vertex buffer + optional index buffer +
texture bindings) and `GECommandBuffer::bindMesh` / `drawMesh` helpers.
Our job in kREATE is the **public Mesh handle** (no GTE in the header),
the **Renderer::draw** entry point that pushes the MVP and issues the
draw, and the **vertex-attribute contract** that links a kREATE mesh
layout to an OmegaSL `buffer<T>` declaration.

The Key Decision from the roadmap — *how per-object uniforms reach the
shader* — is resolved here as **push constants**. Rationale below (§2).

---

## Files

### New

| File | Purpose |
|------|---------|
| `kreate/include/kreate/Mesh.h` | Public Mesh handle (opaque), `MeshDesc`, `VertexAttribute` flag namespace, `MeshTopology`, `IndexFormat`. No GTE includes. |
| `kreate/src/mesh/MeshFactory.h` | Internal factory entry points (parallels `PipelineFactory`). Internal accessor `MeshFactory::geMesh(Mesh&)` for the Renderer. |
| `kreate/src/mesh/Mesh.cpp` | `MeshFactory::create` — allocates an Upload `GEBuffer`, `memcpy`s caller's vertex/index data, builds a `GEMesh`. |
| `kreate/tests/shaders/Phase1Basic.omegasl` | Vertex+fragment shader for `BasicGame`. Position+Color vertex layout, push-constant MVP. |

### Modified

| File | Change |
|------|--------|
| `kreate/include/kreate/Object.h` | Add `setMesh` / `mesh()`. |
| `kreate/src/Object.cpp` | Store the mesh handle. |
| `kreate/include/kreate/App.h` | Add `createMesh(...)` overloads. |
| `kreate/src/App.cpp` | Forward `createMesh` to `MeshFactory`. |
| `kreate/src/pipeline/PipelineFactory.h` | Add internal accessor for the underlying `GERenderPipelineState` so the Renderer can bind it without re-reaching into `Pipeline::Impl`. |
| `kreate/src/pipeline/Pipeline.cpp` | Implement that accessor. |
| `kreate/src/renderer/Renderer.h` | Add `void draw(Pipeline&, Mesh&, const Mat4 &mvp)`. |
| `kreate/src/renderer/Renderer.cpp` | Bind pipeline state, push MVP via `setRenderConstants`, `bindMesh` + `drawMesh`. |
| `kreate/src/Scene.cpp` | Activate the per-object draw loop. |
| `kreate/CMakeLists.txt` | Glob `src/mesh/*.cpp`; stage `tests/shaders/Phase1Basic.omegasl` next to `BasicGame`. |
| `kreate/tests/BasicGame.cpp` | Build the pipeline, build a cube mesh, spawn an Object, spin it. |

---

## 1. Vertex-attribute contract

The contract mirrors `GEMeshVertexAttribute` so that a kREATE mesh can be
flattened to a `GEMesh` without a translation step.

Layout — tightly packed, fixed order, no padding:

```
Position (float3, 12B) → UV2 (float2, 8B) → UV3 (float3, 12B)
                       → Normal (float3, 12B) → Color (float4, 16B)
```

Only the attributes set in `MeshDesc::attributes` are present, but their
relative order is the order above. The OmegaSL `buffer<T>` struct the
caller writes must match the same order/types for the attributes they
enable. Phase 1 ships with **Position + Color** (12B + 16B = 28B
stride); the contract supports the others but the test does not exercise
them yet.

`VertexAttribute` is exposed as a flag namespace (not `enum class`) so
that `Position | Color` is natural without casts — same pattern GTE
uses for `GEMeshVertexAttribute`.

Public surface:

```cpp
namespace Kreate {

namespace VertexAttribute {
    constexpr uint32_t Position = 1u << 0;
    constexpr uint32_t UV2      = 1u << 1;
    constexpr uint32_t UV3      = 1u << 2;
    constexpr uint32_t Normal   = 1u << 3;
    constexpr uint32_t Color    = 1u << 4;
}

enum class MeshTopology : uint8_t { Triangle, TriangleStrip };
enum class IndexFormat  : uint8_t { None, UInt16, UInt32 };

struct KREATE_EXPORT MeshDesc {
    uint32_t      attributes  = VertexAttribute::Position;
    MeshTopology  topology    = MeshTopology::Triangle;
    IndexFormat   indexFormat = IndexFormat::None;
};

class KREATE_EXPORT Mesh {
public:
    ~Mesh();
    Mesh(const Mesh&) = delete;
    Mesh &operator=(const Mesh&) = delete;
private:
    Mesh();
    struct Impl;
    std::unique_ptr<Impl> impl;
    friend struct MeshFactory;
};

KREATE_EXPORT size_t vertexStrideFor(uint32_t attributes);
} // namespace Kreate
```

`vertexStrideFor` is the public helper a caller uses to size their
vertex buffer — mirrors `OmegaGTE::geMeshStrideFor`.

## 2. Per-draw uniforms — **push constants**

Roadmap key decision: how MVP (and later, per-object material params)
reach the shader. Options:

- **Push constants** — `GECommandBuffer::setRenderConstants(data, size)`.
  Already plumbed across D3D12 / Metal / Vulkan, no buffer alloc per
  draw, ≤128 bytes portable. A `Mat4` is 64 bytes — fits.
- Per-draw uniform buffer — needs a ring buffer + binding slot, larger
  blast radius for one Phase-1 number.
- Bindless — overkill before we have a material system.

Push constants it is. The OmegaSL form is `constant<T> pc : N;` with
`[in pc]` opt-in per stage (see `gte/omegasl/tests/push_constant.omegasl`).
The shader's push-constant struct is the kREATE contract; for Phase 1:

```omegasl
struct PushData {
    float4x4 mvp;
};
constant<PushData> pc : 0;
```

The Renderer pushes `sizeof(Mat4) == 64` bytes per draw. Material
parameters fold into the same `PushData` struct later (Phase 4); for
Phase 1 MVP is all that's pushed.

`Renderer::draw` flow:

```
setRenderPipelineState(pipeline.state)
setRenderConstants(&mvp.data[0], sizeof(float) * 16, /*offset*/0)
bindMesh(mesh.geMesh, /*vertexSlot*/0)
drawMesh(mesh.geMesh, /*vertexSlot*/0)
```

The vertex-slot register (`0` in Phase 1) is fixed by the shader's
`buffer<VertexIn> verts : N` annotation. Phase 1 hard-codes `0`; a
later phase can surface it on `MeshDesc` if a mesh needs to bind at a
different register.

## 3. App::createMesh

```cpp
std::shared_ptr<Mesh> App::createMesh(
    const MeshDesc &desc,
    const void *vertexData, size_t vertexBytes, unsigned vertexCount,
    const void *indexData = nullptr, size_t indexBytes = 0,
    unsigned indexCount = 0);
```

Single overload — `indexData == nullptr` matches `indexFormat == None`.
Mismatch (e.g. `indexFormat == UInt16` but `indexData == nullptr`) is
loud: error log + null return, same shape as `PipelineFactory` failure
modes.

Internal: forwards to `MeshFactory::create(impl->renderer->gte(), ...)`.

## 4. Object — mesh slot

`Object` gains `setMesh(shared_ptr<Mesh>)` / `mesh() const`. No other
shape change.

## 5. Scene::render — activate the loop

Replace the commented-out section with the live loop. A visible object
is one with `isVisible()`, a non-null `pipeline()`, AND a non-null
`mesh()` — until all three are present, the object is silently skipped
(this is a Phase-1 pre-condition; later we will sort by pipeline to
amortize state changes, but a single-pipeline phase doesn't need it).

```cpp
for (auto &n : nodes) {
    if (!n.object->isVisible()) continue;
    auto pipe = n.object->pipeline();
    auto mesh = n.object->mesh();
    if (!pipe || !mesh) continue;
    Mat4 mvp = impl->projection * impl->view * n.cachedWorld;
    r.draw(*pipe, *mesh, mvp);
}
```

The world-transform pass above is unchanged.

## 6. Test game — `BasicGame` rewrite

Builds a unit cube (8 vertices × 3-attribute interleave: position +
color, 36 vertices for 12 triangles non-indexed — keep it simple, no
index buffer in Phase 1), spawns one `Object`, rotates it on `onFrame`.
The existing camera (`Mat4::lookAt({0,2,5}, {0,0,0}, {0,1,0})`) frames
it.

Shader path: `Phase1Basic.omegasl` is staged next to the executable by
`KreateGame.cmake` (new `SHADERS` argument — same shape as the
`gte/tests/directx` `ASSETS` list). The test calls
`createPipeline("Phase1Basic.omegasl", desc)`. Pre-compilation into a
`.omegasllib` is a later optimization; runtime compile keeps the test
self-contained.

## 7. Implementation order

Each step is small enough to compile + verify before the next.

1. **Mesh public header + factory skeleton** — `Mesh.h`, `MeshFactory.h`,
   `Mesh.cpp` returning a populated `GEMesh`. KREATE compiles; nothing
   uses it yet.
2. **App::createMesh + Object::setMesh** — wire the factory through the
   App, give Object a mesh slot.
3. **Renderer::draw** — internal accessors on `PipelineFactory` /
   `MeshFactory`, the draw method itself.
4. **Activate Scene::render** — uncomment the loop, fill it in.
5. **Shader + CMake staging** — `Phase1Basic.omegasl`, extend
   `add_kreate_game` with a `SHADERS` arg that `copy_if_different`s next
   to the exe.
6. **BasicGame rewrite** — build the pipeline + cube, spin it.

Each step lands as its own commit; steps 1–4 keep the existing
`BasicGame` (clear-only) working — the test only switches to drawn
geometry in step 6.

## 8. Verification

1. **Build:** `cmake --build` succeeds on the native target.
2. **Header isolation:** `BasicGame.cpp` still compiles with zero GTE
   includes. `Mesh.h` references no GTE types.
3. **Clear-only regression:** through steps 1–4, the unchanged
   `BasicGame` still presents a dark-gray cleared window (the new draw
   loop is a no-op when objects have no mesh).
4. **Cube on screen (the deliverable):** `BasicGame` renders a spinning
   colored cube. Verified by running the app and capturing a screenshot
   (per AGENTS.md *Visual Debugging* — hand off to the user; the
   `omega-debugviz` tool is not yet trusted).
5. **Push-constant correctness:** the cube tracks the per-frame
   transform smoothly (no jitter, no stale MVP); the spin axis matches
   what `onFrame` sets.
6. **Failure modes loud:** `createMesh` with an inconsistent
   index-format / index-data combination logs and returns null; an
   object with mesh but no pipeline (or vice versa) is skipped silently
   in the draw loop (the missing piece is a caller bug, not a runtime
   error).

## 9. Out of scope (next phases)

- Indexed mesh end-to-end test (the API supports it; the test stays
  non-indexed for simplicity).
- Mesh loading from disk (Phase 3 — `GEMeshAsset`).
- Materials (Phase 4) — push constants beyond MVP.
- Pipeline sorting / state-change amortization in `Scene::render`.
- `PrimitiveTopology` on `PipelineDesc` (introduced by the DebugDraw
  plan; not needed for triangle meshes here).
