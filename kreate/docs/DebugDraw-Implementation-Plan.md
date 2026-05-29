# KREATE Debug Draw — Implementation Plan

## Context

kREATE today can **open a window and clear it to a color** — `Scene::render` walks
the graph, computes world transforms, and calls `Renderer::beginFrame` /
`endFrameAndPresent`, but the per-object draw loop is commented out and
`Renderer` has **no `draw()`** (see `Pipeline-Object-Scene-Plan.md` and
`Engine-Roadmap.md` §1). Nothing is drawn yet.

**Debug draw** — world-space lines, wire shapes, and points an engine emits to
make the invisible visible — is the simplest possible renderable: a vertex
buffer, a line pipeline, and one draw call. It needs **none** of the mesh/asset
machinery (`GEMesh`, `GEMeshAsset`, materials) the roadmap's "first mesh on
screen" (Phase 1) waits on. That makes it worth pulling forward, for three
reasons:

1. **It de-risks the mesh path.** Bringing up debug draw exercises the exact
   machinery Phase 1 needs — `Renderer::draw`, vertex buffers, MVP upload,
   pipeline binding, primitive topology — on the *simplest* payload. The draw
   path gets proven before meshes and assets complicate it.
2. **A sibling module is already waiting on it.** AQUA's roadmap states, in
   several places, that **"AQUA emits debug-draw data; kREATE renders it"**
   (`aqua/docs/Physics-Roadmap.md` §6, principle 6). AQUA **Phase 2**'s runnable
   deliverable is literally *"a debug-draw view of many bodies' AABBs with
   overlapping pairs highlighted."* Without a kREATE debug-draw sink, that
   deliverable cannot be shown. kREATE debug draw unblocks it.
3. **The roadmap already values it early, even though it's scheduled late.**
   `Engine-Roadmap.md` lists debug draw under **Phase 16 (Tooling)** — "lines,
   shapes, text in world space (invaluable for physics/AI)" — but principle 5
   ("author for the 3am on-call engineer") and AQUA's principle 6 both argue for
   it from the start. **This plan proposes landing a thin debug-draw slice
   alongside Phase 1** (call it Phase 1.5), not at Phase 16. That reschedule is a
   decision to make deliberately (§9.1), not by default.

This is an **implementation plan**, following the conventions of
`Pipeline-Object-Scene-Plan.md`: concrete files, KREATE-only public types, GTE
behind the implementation boundary, and a runnable deliverable.

**Architecture:**

```
User / sibling code (BasicGame, or kREATE's AQUA-integration layer)
  |  app.debugDraw().line(a, b, color);  debugDraw().wireBox(center, half, rot, color);
Public API: kreate/DebugDraw.h   (Kreate::Vec3 / Color / Mat4 only — no GTE)
  |  implementation boundary
Internal: src/renderer/DebugRenderer.{h,cpp}  (batch -> GEBuffer -> line pass)
  |  only these .cpp include OmegaGTE
OmegaGTE: GECommandBuffer::drawPolygons(Line, ...), setVertexBuffer, makeBuffer,
          RenderPipelineDescriptor::primitiveTopologyCategory = Line
```

GTE already provides everything the renderer side needs (verified):
- `GECommandBuffer::PolygonType { Triangle, …, Line, LineStrip, Point }`
  (`GERenderTarget.h:52-58`), plus `setVertexBuffer(buffer)` and
  `drawPolygons(PolygonType, vertexCount, startIdx)` (`:72`, `:95`).
- `gte.graphicsEngine->makeBuffer({BufferDescriptor::Upload, len, stride})`
  (`GE.h:192`; same call site shape as `gte/tests/matrix_ops_test.cpp:225`).
- `RenderPipelineDescriptor::primitiveTopologyCategory`
  (`GEPipeline.h:153,170`) — defaults to `Triangle`; a line pipeline must set
  `Line`. kREATE's `PipelineDesc` does **not** expose this yet (§3, §9.1).

---

## New Files

| File | Purpose |
|------|---------|
| `kreate/include/kreate/DebugDraw.h` | Public immediate-mode debug-draw API (KREATE types only) |
| `kreate/src/renderer/DebugRenderer.h` | Internal — owns the line vertex buffer, batch, and debug pipeline |
| `kreate/src/renderer/DebugRenderer.cpp` | Impl — includes GTE; uploads the batch, encodes the line pass |
| `kreate/src/DebugDraw.cpp` | Public-API impl — appends primitives to the batch (no GTE) |
| `kreate/shaders/DebugLine.omegasl` | Line vertex+fragment shader (pos+color → view-proj → color) |
| `kreate/tests/DebugDrawGame.cpp` | Runnable deliverable — grid, axis gizmo, spinning wire cube |

## Modified Files

| File | Change |
|------|--------|
| `kreate/include/kreate/Pipeline.h` | Add `PrimitiveTopology { Triangle, Line, Point }` to `PipelineDesc` (§3, §9.1) |
| `kreate/src/pipeline/Pipeline.cpp` | Map `PrimitiveTopology` → `RenderPipelineDescriptor::primitiveTopologyCategory` |
| `kreate/src/renderer/Renderer.h` | Own a `DebugRenderer`; expose `debugRenderer()` to Scene; `drawDebug(viewProj)` step |
| `kreate/src/renderer/Renderer.cpp` | Build the debug pipeline at init; flush the debug batch inside the open pass |
| `kreate/include/kreate/App.h` | Add `DebugDraw &debugDraw();` accessor (forwards to the renderer's batch) |
| `kreate/src/App.cpp` | Wire `debugDraw()` to the internal `DebugRenderer`'s batch |
| `kreate/src/Scene.cpp` | After (future) object draws, before `endFrameAndPresent`, flush debug batch with `projection*view` |
| `kreate/CMakeLists.txt` | Add the new sources; install/compile `shaders/DebugLine.omegasl` |
| `kreate/tests/CMakeLists.txt` (or root) | Build `DebugDrawGame` |

---

## 1. Public API — `DebugDraw`

**Header:** `kreate/include/kreate/DebugDraw.h`

Immediate-mode: every primitive is submitted **per frame** and cleared after the
frame is presented (the Bullet/PhysX debug-draw convention). World space. KREATE
types only — no GTE in the header.

```
DebugDraw                                  // obtained from App::debugDraw()
  // primitives (all take a Color; default duration = this frame)
  line(Vec3 a, Vec3 b, Color c)
  ray(Vec3 origin, Vec3 dir, float len, Color c)
  point(Vec3 p, float size, Color c)        // small 3-axis cross
  wireBox(Vec3 center, Vec3 halfExtents, Mat4 rotation, Color c)   // 12 edges
  wireAABB(Vec3 min, Vec3 max, Color c)                            // axis-aligned
  wireSphere(Vec3 center, float radius, Color c, int rings = 3)    // 3 great circles
  wireCapsule(Vec3 a, Vec3 b, float radius, Color c)               // segment + caps
  arrow(Vec3 from, Vec3 to, Color c)        // line + arrowhead (contact normals)
  transform(const Mat4 &m, float scale)     // RGB axis gizmo at a transform

  // optional persistence (§9.3): keep drawing for `seconds` without re-submitting
  // line(a, b, c, float seconds);  // overload — decays via Time (Phase 5)

  clear()                                   // drop the current batch (rarely needed)
```

`DebugDraw` is a thin façade over the internal batch; the wire-shape helpers
(`wireBox`, `wireSphere`, …) decompose into `line` calls in `DebugDraw.cpp`, so
the renderer only ever sees a flat list of colored line segments. Pure data — no
GPU work happens here.

---

## 2. Vertex format & the CPU batch

**Internal:** `DebugRenderer.h` (not public).

```
DebugVertex { float x, y, z;  float r, g, b, a; }     // pos + color, 28 bytes
```

The batch is a `std::vector<DebugVertex>` of line endpoints (two vertices per
segment). `DebugDraw` appends into it; `DebugRenderer` consumes it at flush. The
vertex-attribute order (`position` then `color`) is the contract with
`DebugLine.omegasl` (§3), mirroring how Phase 1 will define the
mesh↔shader attribute contract (`Engine-Roadmap.md` Phase 1).

**Buffer strategy (avoid per-frame allocation, cross-cutting §5 of the roadmap):**
keep one persistent `GEBuffer` (`BufferDescriptor::Upload`) sized to a high-water
mark; each frame, `memcpy` the batch in and grow (reallocate ×2) only when the
batch exceeds capacity. The buffer is `Upload` because the CPU rewrites it every
frame.

---

## 3. The debug pipeline & line topology

**Shader:** `kreate/shaders/DebugLine.omegasl` — a minimal pass-through:
- **vertex:** `clipPos = viewProj * float4(position, 1); outColor = color;`
  (`viewProj` delivered as a `uniform` — see §4 binding).
- **fragment:** `return color;`

**Pipeline:** built once at renderer init through the existing
`Pipeline`/`PipelineFactory` path, with:
- `fillMode = Solid`, `cullMode = None` (lines aren't culled),
- `enableDepth = true` (depth-tested so lines occlude correctly; an overlay mode
  is §9.2),
- **`primitiveTopology = Line`** — the new `PipelineDesc` field, mapped in
  `Pipeline.cpp` to `RenderPipelineDescriptor::primitiveTopologyCategory =
  PrimitiveTopologyCategory::Line` (`GEPipeline.h:170`). Without this the D3D12
  PSO is created for triangles and line draws are invalid.

> **`PipelineDesc` change (§9.1).** Add `PrimitiveTopology { Triangle, Line,
> Point }` defaulting to `Triangle`, so existing pipelines are unchanged and the
> debug pipeline is just `{ .primitiveTopology = Line, .fillMode = Solid,
> .cullMode = None, .enableDepth = true, .vertexFunction = "debugLineVS",
> .fragmentFunction = "debugLineFS" }`. This field is needed for *any* line/point
> rendering, not only debug draw, so it belongs on the public surface rather than
> hidden in a one-off internal pipeline.

---

## 4. Renderer changes

**`Renderer` (internal)** gains ownership of a `DebugRenderer` and a flush step
inside the open render pass:

```
Renderer
  beginFrame(clearColor)                 // unchanged
  // NEW:
  DebugRenderer &debug();                // Scene/App reach the batch through this
  void flushDebug(const Mat4 &viewProj); // upload batch, set line PSO, setVertexBuffer,
                                          //   bind viewProj uniform, drawPolygons(Line, n, 0)
  endFrameAndPresent()                   // unchanged
```

`flushDebug` runs **inside** the current pass (between `startRenderPass` and
`finishRenderPass`), so it shares the scene's render target and depth buffer.
Steps (in `DebugRenderer.cpp`, the only new GTE TU):
1. If the batch is empty, return.
2. Grow/reuse the `Upload` vertex buffer (§2); `memcpy` the batch.
3. `cmd->setRenderPipelineState(debugPSO)`.
4. `cmd->bindResourceAtVertexShader(viewProjUniformBuffer, slot)` — upload the
   `Mat4` view-proj into a small `Uniform` buffer first.
5. `cmd->setVertexBuffer(vertexBuffer)`.
6. `cmd->drawPolygons(GECommandBuffer::Line, vertexCount, 0)`.
7. Clear the batch (immediate-mode), unless the primitive has remaining duration
   (§9.3).

The `Renderer` builds `debugPSO` once during `create()` via the pipeline factory,
reusing the GTE instance it already owns.

---

## 5. Scene / App integration

- **`App::debugDraw()`** returns the `DebugDraw` façade bound to the renderer's
  batch, so game code and (later) the AQUA-integration layer submit primitives
  anywhere during `onFrame`.
- **`Scene::render`** flushes the batch after the (future) object draws and before
  present, using the camera it already holds:
  ```
  r.beginFrame(clearColor);
  // ... (future) object draws ...
  Mat4 viewProj = projection * view;       // Scene already stores both
  r.flushDebug(viewProj);
  r.endFrameAndPresent();
  ```
  Until the object-draw loop lands, the scene **clears, draws debug lines, and
  presents** — which is exactly the deliverable (§12) and kREATE's first geometry
  on screen.

---

## 6. Math helpers needed

kREATE's `Math` is "the minimum a camera needs" (`Math.h`: `Vec3`, `Vec4`,
`Color`, `Mat4`). Debug draw needs only a little more, all in the *generation*
helpers (`DebugDraw.cpp`), not the public header:
- `Vec3` add/sub/scale and a transform-point-by-`Mat4` (for `wireBox`/`transform`).
- Wire-shape vertex generation (box's 12 edges, sphere's 3 great circles, capsule
  segment+caps) — pure endpoint math, no new types.

This nudges the **Phase 2 math work** (`Engine-Roadmap.md` Phase 2: `Vec3` ops,
`Transform`, `AABB`) a little earlier, but only the handful of ops the generators
need — not the full library. Flagged so it's a deliberate small pull, not scope
creep.

---

## 7. The AQUA bridge (cross-module boundary)

AQUA's roadmap assigns it to *emit* debug-draw data and kREATE to *render* it, but
the modules must not hard-depend on each other's types: **AQUA is consumed by
kREATE, so AQUA cannot include kREATE headers** (it cannot call
`Kreate::DebugDraw`). The clean seam:

- **AQUA exposes neutral debug primitives** — its own value types (e.g.
  `AQDebugLine { OmegaGTE::FVec<3> a, b; float rgba[4]; }`) drained from the
  space each frame. *This is a small follow-up on AQUA's side and does not exist
  yet* — surfaced here as a cross-module dependency rather than assumed.
- **The adapter lives in kREATE**, in the future physics-integration layer
  (`Engine-Roadmap.md` **Phase 8**, which already depends on AQUA): it reads
  AQUA's debug primitives and replays them as `DebugDraw::line` calls, converting
  `OmegaGTE::FVec<3>` → `Kreate::Vec3` at the boundary (kREATE already links GTE,
  so the borrowed math type costs nothing — same boundary rule as
  `Physics-Roadmap.md` §6).

So this plan delivers the **kREATE sink** now; the AQUA→kREATE adapter is a thin
Phase-8 (or earlier, when AQUA Phase 2 wants its deliverable) addition once AQUA
grows its neutral debug surface. Neither module gains a new hard dependency.

---

## 8. CMakeLists changes

- Add `src/DebugDraw.cpp`, `src/renderer/DebugRenderer.cpp` to KREATE `SOURCES`.
- Compile/stage `shaders/DebugLine.omegasl` (runtime-compiled via the existing
  `App::createPipeline` path, or pre-built into the `.omegasllib` like other
  shaders).
- Add the `DebugDrawGame` test target (mirrors `BasicGame`).

---

## 9. Key decisions

1. **Reschedule from Phase 16 → Phase 1.5, and add `PrimitiveTopology` to
   `PipelineDesc`.** *Lean: yes to both.* Debug draw is cheap, de-risks the Phase
   1 mesh path, and unblocks AQUA Phase 2's deliverable; the topology field is
   needed for any line/point rendering regardless. The alternative (build a
   one-off internal line pipeline, leave `PipelineDesc` alone) avoids a public
   change but hides a generally-useful capability — flagged, not chosen
   unilaterally.
2. **Depth-tested vs. overlay.** *Lean: depth-tested by default* (lines occlude
   correctly against scene geometry), with an **overlay** mode (depth test off,
   drawn on top) as a later flag — physics debugging often wants "always visible"
   contact normals. One PSO now, a second when needed.
3. **Immediate-mode vs. retained/persistent.** *Lean: immediate-mode* (clear each
   frame), with an optional `seconds` duration overload that decays against the
   Phase 5 `Time` clock — persistent lines are invaluable for transient events
   (a single-frame raycast hit) but require the clock, so they land when it does.
4. **Vertex layout / topology granularity.** *Lean: a flat `Line` list*
   (two vertices per segment) over `LineStrip` — simplest batching, no
   primitive-restart, and every wire shape decomposes to independent segments.
5. **Text in world space.** *Lean: out of scope here* — labels (body indices,
   contact ids) are genuinely useful but pull in glyph rendering; defer to the
   Phase 12 UI / Phase 16 tooling work and keep this slice to lines+points.
6. **Where the AQUA neutral debug surface lives.** Not a kREATE decision, but a
   dependency this plan surfaces (§7): AQUA must expose drainable debug primitives
   for the kREATE adapter to consume.

---

## 10. Implementation order

1. **`PipelineDesc` topology field** + `Pipeline.cpp` mapping (smallest, unblocks
   the line PSO).
2. **`DebugLine.omegasl`** + build the debug PSO in `Renderer::create`.
3. **`DebugVertex` + batch + `GEBuffer` upload** in `DebugRenderer`.
4. **`Renderer::flushDebug`** — set PSO, bind view-proj, `setVertexBuffer`,
   `drawPolygons(Line, …)`.
5. **`DebugDraw.h` façade + `DebugDraw.cpp`** generators (line → wire shapes).
6. **`App::debugDraw()`** accessor; **`Scene::render`** flush step.
7. **`DebugDrawGame.cpp`** — grid + axis gizmo + spinning wire cube.
8. **CMake** wiring.

---

## 11. GTE isolation summary

| KREATE public header | GTE types referenced | Status |
|---|---|---|
| `DebugDraw.h` | None — `Kreate::Vec3` / `Color` / `Mat4` only | New, isolated |
| `Pipeline.h` | None — adds `PrimitiveTopology` enum (KREATE) | Modified, still isolated |
| `App.h` | None — adds `DebugDraw &debugDraw()` | Modified, still isolated |

**Only these `.cpp` include GTE:** `src/renderer/DebugRenderer.cpp` (buffers,
pipeline, `drawPolygons`), alongside the existing GTE TUs
(`Renderer.cpp`, `Pipeline.cpp`, `App.cpp`, `Math.cpp`). `DebugDraw.cpp` and the
test are GTE-free.

---

## 12. Verification

1. **Build:** `cmake --build` succeeds with the new sources and shader.
2. **Header isolation:** `DebugDrawGame.cpp` compiles with **zero** GTE includes;
   no GTE leaks through `DebugDraw.h` / `Pipeline.h` / `App.h`.
3. **First geometry on screen:** `DebugDrawGame` draws a **ground grid, an RGB
   axis gizmo at the origin, and a wireframe cube rotating per frame** — kREATE's
   first rendered geometry. Visual confirm on the host backend (Metal).
4. **Depth correctness:** the cube's far edges are occluded by its near faces
   when a solid object is present (or, with grid only, lines behind the camera
   are clipped) — confirms `viewProj` and depth test.
5. **Topology:** the line PSO is created with `primitiveTopologyCategory = Line`;
   triangle pipelines are unaffected (existing `BasicGame` still clears/presents).
6. **Batch lifetime:** with no `debugDraw()` calls in a frame, the pass draws
   nothing (empty batch early-out); submitting N lines draws exactly N segments.
7. **Buffer reuse:** the vertex buffer is allocated once and reused across frames;
   it grows only when the batch exceeds capacity (no per-frame allocation).
8. **AQUA payoff (later, gated on §7):** once AQUA exposes neutral debug
   primitives, the kREATE adapter renders AQUA Phase 2's **AABBs + highlighted
   overlapping pairs** through this exact path — the cross-module deliverable.

---

*This is a living plan. It proposes pulling debug draw forward from
`Engine-Roadmap.md` Phase 16 to a thin Phase-1.5 slice because it de-risks the
mesh path and unblocks AQUA's visual deliverables; that reschedule (§9.1) is the
one decision to confirm before building.*
