# AQUA Phase 2.x — Static Triangle-Mesh Colliders

**Prior-art brief & proposal.** This is the research artifact that §4 of
`Physics-Roadmap.md` requires before a subsystem is written, for the extension
Phase 2 explicitly parked: its §11.6 lands sphere / box / capsule / plane /
convex hull and defers "heightfield and static triangle mesh … to a Phase 2.x /
the static side of Phase 3." That deferral is now the pillar-blocking gap: level
geometry in kREATE is triangle meshes, not unions of convex primitives. This
brief covers **static (non-simulated) triangle-mesh colliders only** — giving
the *world* geometry, and letting every consumer AQUA has since grown collide
against it: rigid bodies (Phase 3 manifolds), queries + CCD (Phase 4),
particles (Phase 6 push-out, CPU and GPU), and — the timeliness driver — cloth
against level geometry (Phase 8). No deformable meshes, no mesh-vs-mesh, no
moving meshes; §11 records each as a decision, not an omission.

*Status: proposal, parked in `.plans/future/` until greenlit. Per the house
discipline (Phase 6 §13 / Phase 7 §13), an implementation-contract §13 gets
written against ground-truth source when work begins; where this proposal and
that future §13 disagree, §13 will govern.*

---

## 1. Scope & deliverable

**Goal.** Let a space carry cooked, immovable triangle meshes as first-class
colliders: a body, particle, ray, or shape-cast near a 100k-triangle mesh must
find its *handful* of relevant triangles in ~log time, produce contacts that do
not snag on interior edges, and never, ever tunnel the floor.

**Runnable deliverable.** A **playground scene**: rigid bodies (spheres, boxes,
capsules) dropped onto and sliding across a non-convex cooked mesh (a terrain
patch with a bowl and a ramp, plus a *flat floor deliberately tessellated into
many triangles*), a Phase 6 particle fountain raining onto the same mesh, and
rays cast through it — with a headless test holding four oracles at once:

- **Midphase exactness.** The candidate-triangle set returned for a query AABB
  is a superset of the brute-force all-triangles AABB-overlap set — *zero
  missed triangles* (the §2 catastrophe class), false positives bounded by the
  cooked node fattening. The brute-force scan is this phase's `double` oracle.
- **The no-snag invariant (the headline).** A box sliding across the flat
  *tessellated* floor must behave exactly as it does on the analytic plane:
  every contact normal within a stated ε of the plane normal, and lateral
  velocity drift below a band over the whole slide. This is the internal-edge
  ghost-collision failure mode (§2) tested directly — the single test that
  separates a usable mesh collider from a demo.
- **No tunneling.** No live particle ever found meaningfully behind a floor
  triangle's surface (the Phase 6 §9 hard invariant, pointed at a mesh), and
  the Phase 4 CCD bullet cannot pass through the mesh.
- **Raycast parity.** BVH-traversed raycasts return the same nearest hit as a
  brute-force scan over every triangle, with deterministic tie-breaking.

**Included groundwork (lands first).** A deterministic **cooking** pass —
vertex weld, degenerate-triangle rejection (loud), edge-adjacency +
active-edge classification, and the midphase BVH build — because every runtime
property above is a property of the *cooked* data. Cooking is in-engine and
byte-deterministic (§11.7): same input mesh ⇒ identical cooked pools.

**What the shipped substrate already gives us (audited against source,
2026-07-07).**
- **Phase 2** shipped `AQShape` (POD tagged union, `AQCollision.h`) with the
  pooled-storage precedent this extension generalizes: the convex hull stores
  `(firstVertex, vertexCount)` into a space-owned `hullVerts` pool
  (`AQSpaceImpl.h`) rather than inline data. Mesh colliders extend that idiom
  with cooked triangle/node/adjacency pools. The plane case also set the
  broadphase precedent §6.B leans on: a shape whose extent is "the world" is
  handled specially rather than jammed into the grid's cell-size bound.
- **Phase 3** shipped the specialized-pair branch table + GJK/EPA fallback
  (`AQNarrowphase.cpp`, `AQGJK.cpp` — Nesterov-accelerated per the Phase 3
  audit), manifold reduction to the 4 deepest points, and the warm-start
  cache. Per-triangle narrowphase (§6.D) *reuses* this: a triangle is a
  3-vertex convex fed to the same machinery, not a new solver.
- **Phase 4** shipped `raycast` / `shapecast` / `overlap` and the swept-sphere
  CCD pass — the query consumers §6.F extends to mesh leaves.
- **Phase 6** shipped the scalar-generic point-vs-shape signed distance
  (`AQParticleCollision.h`, hull → `+inf`) and, in the 6h/6i GPU sub-phase,
  the live on-device collide kernel over analytic shapes. §6.E gives particles
  a mesh distance query on the CPU path first; the kernel branch follows the
  house stage-isolation pattern.
- **Phase 2 §12 (recency audit)** already selected the BVH build lineage this
  brief adopts for cooking: PLOC/PLOC++ (Meister & Bittner 2018/2022), "so the
  eventual BVH path skips a generation" past Karras 2012. This is that path.

**Out of scope here, by design.** Deformable/skinned meshes (Phase 9's
tetrahedral world is the deformable answer), mesh-vs-mesh (static-static is
never simulated — §11.8 records it as permanent), moving/kinematic meshes
(§11.5 decision, lean static-strict), heightfields (§11.6 — a separate,
cheaper specialized type once meshes prove the interfaces), per-triangle
materials (a follow-on field on the cooked triangle record), and the GPU
midphase kernel (designed-for here, landed later behind stage-isolation parity
like every GPU stage before it).

---

## 2. Why mesh collision is its own problem

1. **A mesh is not a shape; it is a container of shapes.** Everything Phase 2/3
   built assumes a convex query object: one support function, one GJK/EPA call,
   one manifold. A triangle mesh is a *soup* of convex triangles — collision
   against it is a two-stage problem: *find the few relevant triangles*
   (midphase), then run per-triangle convex tests. The midphase is a broadphase
   that lives *inside* a shape, and it needs its own acceleration structure,
   its own exactness oracle, and its own determinism story.
2. **The internal-edge problem is the defining failure mode.** Two adjacent
   coplanar triangles share an edge; a box sliding across them contacts the
   *interior* edge of one triangle and receives a contact normal pointing
   sideways along the edge's Voronoi region instead of up along the surface.
   The result is the classic mesh-collider bug: bodies snag, hop, and catch on
   perfectly flat tessellated floors. It cannot be fixed at solve time; it
   must be fixed with *cooked adjacency* — knowing, per edge, whether it is a
   real feature (convex crease) or an internal artifact of tessellation, and
   clamping contact normals accordingly. Every mature engine carries exactly
   this machinery; a mesh collider without it is not shippable.
3. **The scale asymmetry inverts the cost model.** Phase 2's shapes are O(1)
   records; a level mesh is 10⁴–10⁶ triangles with *one* body touching ~10 of
   them. Testing a body against every triangle is the O(n²) mistake of §2 of
   the Phase 2 brief wearing a new hat; and the mesh's single world AABB is so
   large it would also poison the uniform grid's cell-size completeness bound
   (`cellSize ≥ max fat extent` — the 27-neighborhood rule from
   `AQBroadphase.omegasl`). The mesh must therefore *bypass* the grid and
   bring its own log-time structure (§6.B/C).
4. **Cooking introduces a lifecycle stage AQUA has never had.** Analytic shapes
   are authored as raw parameters; a mesh must be *processed* — welded,
   validated, adjacency-classified, BVH-built — before it is usable, and the
   processing choices (weld tolerance, quantization, build heuristic) are
   correctness- and determinism-relevant, not cosmetic. Cooked data is also
   exactly what the GPU path uploads, so its layout is a §8 decision made once.

---

## 3. Prior art — how the incumbents solve it

Studied to understand the terrain and the failure modes, **not** to transcribe.
Descriptions are representative of published docs/talks and may lag current
internals.

**NVIDIA PhysX 5.**
- Triangle meshes are **cooked** offline or at runtime into a quantized,
  4-wide midphase BVH (the BVH33/BVH34 lineage): compressed AABB nodes, leaves
  holding small triangle batches, per-triangle adjacency retained for contact
  normal correction. Cooked meshes are shared/instanced like all PhysX shapes.
- Mesh colliders are **static or kinematic only** against dynamics — a dynamic
  rigid body cannot *wear* a trimesh; the engine steers authors to convex
  decomposition for dynamic props. (PhysX 5 separately grew SDF-voxelized
  trimesh collision for its deformable/dynamic-mesh path — see §12.)
- Contact generation runs per candidate triangle through the same convex
  machinery, followed by internal-edge filtering and manifold reduction.

**Epic Chaos (Unreal Engine 5).**
- UE's **simple/complex split** is the load-bearing idea: "simple" (convex
  primitives) for dynamics, "complex" (the trimesh) for queries and
  static-world collision. The trimesh carries its own midphase hierarchy;
  per-poly collision against dynamic bodies is possible but steered away from.
- The practical lesson AQUA takes: mesh colliders exist primarily to be *the
  world* — the immovable thing everything else collides with, raycasts
  against, and walks on — not to be dynamic bodies.

**Bullet.**
- `btBvhTriangleMeshShape` (quantized AABB tree over triangles) plus the
  canonical internal-edge fix: a cooked edge-adjacency map
  (`btGenerateInternalEdgeInfo`) and a contact-time normal adjustment
  (`btAdjustInternalEdgeContacts`) that snaps illegal edge normals back into
  the cooked normal cone. Bullet is the clearest public statement of §2's
  failure mode and its cure, and the lineage §6.D adopts.

**The shared shape.** All three converge on: cook once into a quantized BVH +
adjacency; static-only against dynamics; per-triangle convex narrowphase
through the engine's existing machinery; internal-edge filtering from cooked
adjacency; manifold reduction after gathering per-triangle contacts. The
disagreements are at the edges (wide vs binary nodes, kinematic support,
whether particles get exact or approximate mesh collision) — and those edges
are exactly AQUA's §11 decisions.

---

## 4. The literature we build on

- **Ericson 2004, *Real-Time Collision Detection*.** The reference for the
  primitive tests this phase needs in closed form — point/segment/triangle
  closest features, triangle-AABB (via Akenine-Möller's SAT), Voronoi-region
  reasoning for edge/vertex contacts — and for BVH construction/traversal
  fundamentals.
- **Möller & Trumbore 1997, "Fast, Minimum Storage Ray-Triangle
  Intersection."** The ray-triangle test for §6.F raycasts: branch-light,
  storage-free, exactly what both a CPU loop and a future kernel want.
- **Akenine-Möller 2001, "Fast 3D Triangle-Box Overlap Testing."** The
  SAT-based triangle-vs-AABB test the midphase leaf visit and the cooking
  binner both use.
- **van den Bergen 1997, "Efficient Collision Detection of Complex Deformable
  Models using AABB Trees."** The AABB-tree-over-triangles midphase in its
  original form, and the argument for AABBs (cheap refit, cheap overlap) over
  OBBs at this layer.
- **Meister & Bittner 2018 (PLOC) / Benthin et al. 2022 (PLOC++).** The
  BVH-construction line Phase 2 §12 already designated as AQUA's build
  algorithm — parallel locally-ordered clustering gives near-SAH quality at
  sort-like cost, and (unlike top-down SAH) is the same algorithm a future
  GPU cooking path would run. Cooking here is CPU-first, but choosing the
  GPU-shaped builder now is the Phase 5 lesson applied.
- **Ylitie, Karras, Laine 2017, "Efficient Incoherent Ray Traversal on GPUs
  Through Compressed Wide BVHs."** The quantization/compression reference for
  the §11.1 wide-node option: child AABBs stored as 8-bit offsets against a
  parent frame. Rendering lineage, but the encoding is exactly what a
  bandwidth-bound midphase kernel wants; recorded for the GPU port, not the
  first cut.
- **Bullet's internal-edge machinery (Coumans, source + docs).** Practice, not
  paper: the cooked active-edge classification and contact-normal clamping
  that §6.D transliterates in spirit. The published-engine consensus *is* the
  literature for this sub-problem.

**Throughline.** The primitive tests are 1997–2004 settled art; the midphase
structure question is the same BVH-vs-grid fork Phase 2 already adjudicated
(grid for uniform dynamic sets, BVH for static size-variant sets — a mesh is
the second case, so the BVH line wins *here* without contradicting the grid
choice *there*); and the build algorithm is the one Phase 2 §12 pre-selected.
Nothing in this phase requires new numerics — it requires new *bookkeeping*
(cooking, adjacency, traversal) done with the determinism discipline the
engine already lives by.

---

## 5. Where AQUA diverges — the openings

- **One pooled-POD vocabulary, extended — no new ownership model.** The hull's
  `(firstVertex, vertexCount)`-into-a-pool pattern becomes the mesh's
  `(meshIndex)`-into-cooked-pools: vertices, triangle records (with adjacency
  bits), BVH nodes — all trivially-copyable SoA arrays owned by the space,
  referenced by the same `AQShapeHandle` machinery, uploadable to GTE buffers
  with no repacking. No virtuals, no pointers, no per-mesh heap objects on the
  hot path. The GPU port of the midphase becomes a buffer bind, not a
  marshalling project.
- **Exact particle-vs-mesh, not screen-space approximation.** Phase 6 §5 made
  exact analytic push-out a differentiator vs Niagara's depth-buffer
  collision; meshes keep that promise: the same cooked BVH answers a
  closest-triangle distance query (`AQParticleCollision.h` grows a mesh case),
  so particles — and later cloth vertices, which are the same query — get
  feature-exact push-out on CPU and, later, on the device.
- **Determinism as a first-class property of cooking AND traversal.** Cooked
  output is byte-deterministic (fixed weld order, fixed tie-breaking in the
  build — no hash-map iteration order anywhere), and midphase candidates are
  emitted in ascending cooked-triangle order so Phase 3 consumes triangles in
  a fixed sequence — the same stable-cross-path, bitwise-within-path stance
  every pillar carries. Incumbent cookers do not promise determinism across
  versions; AQUA's does, because the §9 oracles and the future GPU parity
  tests depend on it.
- **Honest static-only enforcement.** A dynamic body wearing a mesh shape is
  *rejected loudly* at `addBody` (invalid handle + stderr naming the reason),
  not silently degraded to kinematic or given a nonsense inertia (the hull's
  solid-box-approximation fallback must not stretch to meshes). The
  simple/complex lesson from Chaos, stated as a contract instead of a
  convention.
- **The world set beside the grid, by precedent.** Phase 2's plane case
  already established that "world-sized" colliders are special-cased rather
  than forced through the uniform grid. Meshes formalize it: a small
  **static world set** tested body-fat-AABB vs mesh-root-AABB directly
  (meshes are few; bodies are many), keeping the grid's cell-size
  completeness bound untouched (§2.3, §6.B).

**Gaps we must fill.** (a) the cooking pass (weld / validate / adjacency /
build); (b) the midphase BVH + deterministic traversal; (c) triangle-vs-convex
narrowphase glue into the Phase 3 branch table, with internal-edge normal
filtering and cross-triangle manifold reduction; (d) the point-vs-mesh
distance query for particles; (e) mesh cases in raycast / shapecast / overlap
/ CCD; (f) the static world set beside the grid; (g) new debug bits. None of
it is new numerics; all of it is new machinery over settled primitives.

---

## 6. Proposed algorithm — cook once, descend everywhere

**A. Cooking (at `createTriangleMeshShape`, CPU, deterministic).**
```
1. weld vertices within tolerance (§11.7) — first-occurrence keeps its index
   (deterministic remap; no spatial-hash iteration order leaks)
2. reject degenerate triangles (zero area / repeated indices) — LOUDLY, with
   a per-mesh count; never silently keep a zero-normal triangle
3. build edge adjacency: for each edge, its one or two triangles; classify
   each edge ACTIVE (boundary edge, or dihedral angle past a crease
   threshold) or INTERNAL (tessellation artifact); store 3 bits/triangle
   plus the one-ring normal cone data the contact filter needs
4. build the midphase BVH over triangle AABBs (PLOC-style agglomerative or
   binned-SAH — §11.1; fixed tie-breaking), leaves holding 1-4 triangles;
   record the cooked depth bound (the future kernel's fixed traversal stack)
5. emit pools: meshVerts[], meshTris[] (indices + edge bits), meshNodes[],
   per-mesh directory record (root, counts, world AABB)
```

**B. Broadphase — the static world set.** Cooked meshes do not enter the
uniform grid (their AABB would blow the cell-size completeness bound — §2.3).
The space keeps a small array of static mesh colliders; once per advance, each
dynamic body whose fat AABB overlaps a mesh's root AABB emits a `(body, mesh)`
candidate — O(bodies × meshes) with meshes counted in single digits, ordered
`(body, mesh)` ascending for determinism. The plane precedent, made explicit.

**C. Midphase — deterministic BVH descent.** For each `(body, mesh)`
candidate: transform the body's fat AABB into mesh space **once** (the mesh is
static; its inverse transform is cooked), descend the BVH front-to-back in
node order, test leaf triangles with the Akenine-Möller SAT, and emit
candidate triangle indices **ascending**. A loud guard fires when one body's
candidate count exceeds a threshold (a body-sized-like-the-mesh smell — the
broadphase guard idiom from Phase 2 §9).

**D. Narrowphase — triangles through the existing machinery.** Each candidate
triangle is a 3-vertex convex: sphere-tri and capsule-tri get their Ericson
closed forms (cheap, exact); box-tri and hull-tri run the existing GJK/EPA
path with a triangle support function. Then the two mesh-specific steps:
- **Internal-edge filter (the §2.2 cure).** A contact whose feature is an
  INTERNAL edge or vertex has its normal clamped into the cooked normal cone
  of the owning face(s) — Bullet's `btAdjustInternalEdgeContacts` in spirit,
  operating on our cooked bits. Contacts on ACTIVE edges keep their Voronoi
  normal (real creases must still act like creases).
- **Cross-triangle manifold reduction.** All surviving contacts for the
  `(body, mesh)` pair gather into one manifold and reduce with the existing
  4-deepest selection, with feature keys carrying the triangle index so the
  Phase 3 warm-start cache keys stay stable frame to frame.

**E. Particles (and later cloth vertices) — closest-triangle query.** The same
BVH answers a point query: nearest triangle within radius `r`, distance
measured to the triangle's closest feature, push-out along the triangle
normal's side of the surface (one-sided — §11.4). This slots into the Phase 6
collide pass as one more collider kind on the CPU path; the GPU kernel branch
follows behind stage-isolation parity when scheduled (the 6i pattern: the CPU
template is the transcription spec).

**F. Queries + CCD.** `raycast`: BVH descent + Möller–Trumbore, nearest hit,
ties broken by lowest triangle index. `shapecast` / CCD swept-sphere: the
existing conservative sweep gains a mesh case via the same descent with the
sweep's inflated AABB. `overlap`: midphase candidates + the §6.D tests.

**G. GPU (designed-for now, landed later).** The cooked pools upload verbatim
(SoA PODs); the midphase kernel is a fixed-size-stack BVH walk (the cooked
depth bound from §6.A.4 sizes the stack — no dynamic allocation, no
recursion); candidate emission reuses the count → 6g-scan → build shape the
Phase 5e narrowphase already established. Explicitly NOT in the first cut —
the engine's house pattern is CPU-live first, kernels behind parity tests.

**Alternative considered — voxel SDF mesh collision (the PhysX 5 deformable
line).** Voxelize the mesh into a signed-distance volume and collide
everything against the SDF — uniform-cost queries, no midphase, and a natural
fit for AQUA's existing SDF vocabulary. Rejected as the *primary* path: memory
scales with volume not surface, thin features alias at feasible resolutions,
and the no-snag property degrades into the voxel filter. Recorded in §12 as
the representation to revisit for *dynamic* mesh colliders if Phase 9 ever
needs them.

---

## 7. New types AQUA must add — `include/aqua/AQCollision.h` additions (draft)

AQUA-owned, AQ-prefixed, no namespace. POD / trivially-copyable /
standard-layout; raw floats and uints only (the union rule — no FVec members).
The tag is APPENDED, never renumbered (the debug-bits discipline).

```cpp
// --- AQShapeType gains one value (appended) ---
enum class AQShapeType : std::uint32_t {
    Sphere, Box, Capsule, Plane, ConvexHull,
    TriangleMesh,                       // Phase 2.x — cooked static mesh
};

// --- AQShape union gains one variant ---
//   struct { std::uint32_t meshIndex; } mesh;   // into the space's cooked-mesh directory

// --- Cooking input (caller-owned arrays; copied/cooked at create time) ---
struct AQMeshDesc {
    const OmegaGTE::FVec<3> *vertices  = nullptr;  ///< [vertexCount]
    std::size_t              vertexCount = 0;
    const std::uint32_t     *indices   = nullptr;  ///< [indexCount], CCW triples
    std::size_t              indexCount  = 0;      ///< multiple of 3
    float weldTolerance   = 1e-4f;   ///< vertex-merge distance (§11.7)
    float creaseAngleRad  = 0.35f;   ///< dihedral past this ⇒ ACTIVE edge (~20°)
};

// --- Cooked records (space-owned pools; GPU-uploadable SoA) ---
// One triangle: vertex indices + per-edge ACTIVE bits (edge i = v[i]→v[i+1])
// packed into the high bits of `flags`; low bits reserved (per-tri material
// is the recorded follow-on).
struct AQMeshTriangle {
    std::uint32_t v0, v1, v2;
    std::uint32_t flags;            ///< bit0..2 = edge-active, rest reserved
};

// One midphase node (binary first cut, full-precision bounds — §11.1 records
// the quantized/wide upgrade for the GPU port). Leaf iff triCount > 0.
struct AQMeshBVHNode {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
    std::uint32_t leftOrFirstTri;   ///< interior: left child (right = +1 sibling
                                    ///< or stored index — settled at impl); leaf: first tri
    std::uint32_t triCount;         ///< 0 = interior
};

// Per-mesh directory record (root node, pool ranges, cooked stats).
struct AQMeshCollider {
    std::uint32_t firstVertex, vertexCount;
    std::uint32_t firstTri,    triCount;
    std::uint32_t firstNode,   nodeCount;
    std::uint32_t maxDepth;         ///< cooked traversal-stack bound (§6.G)
};

static_assert(std::is_trivially_copyable<AQMeshTriangle>::value, "...");
static_assert(std::is_trivially_copyable<AQMeshBVHNode>::value, "...");
static_assert(std::is_trivially_copyable<AQMeshCollider>::value, "...");
```

The adjacency *cone* data the §6.D filter consumes (per-edge neighbor normals)
lives in a cooked side pool keyed by triangle — internal (`src/`), not public:
callers author `AQMeshDesc` and hold an `AQShapeHandle`; nothing else crosses
the boundary.

---

## 8. Data layout & GPU/numeric specialization

- **Pools, extending the hull precedent.** `AQSpace::Impl` grows
  `meshVerts[]` / `meshTris[]` / `meshNodes[]` / `meshColliders[]` (+ the
  internal adjacency pool), exactly parallel to `hullVerts`. Cooked once at
  create; never touched by the step. Static ⇒ no refit path, no per-frame
  upload — the future GPU port uploads on create only (the cheapest possible
  residency story in the engine).
- **Cooking is float, deterministically ordered.** No `double` cooking path:
  the §9 oracles are set/invariant oracles (candidate supersets, normal
  bands, no-tunnel), not precision oracles, so the Phase 1/6 dual-precision
  discipline does not apply to the *build* — it applies to the *tests*, whose
  brute-force references use the same cooked data.
- **Traversal is allocation-free.** CPU descent uses a fixed stack sized by
  the cooked `maxDepth`; the GPU kernel will use the same bound as a local
  array. Candidate emission ascending by triangle index on every path.
- **Determinism.** Cooked pools byte-identical for identical input; `(body,
  mesh)` candidates and per-pair triangle candidates in fixed ascending
  order; contact feature keys carry triangle indices so warm-start caching
  and the solve order stay reproducible. Stable-cross-path,
  bitwise-within-path — unchanged stance, new territory.

---

## 9. Validation — how we measure "better"

The four deliverable oracles from §1, plus the guards:

- **Midphase exactness (headline #1).** Randomized query AABBs over randomized
  and authored meshes: candidates ⊇ brute-force triangle-AABB overlaps (zero
  false negatives), false positives bounded by node slop. Thousands of cases;
  the brute-force scan is the oracle.
- **No-snag (headline #2).** The tessellated-flat-floor slide: contact normals
  within ε of the plane normal, lateral drift within a band vs the same slide
  on the analytic plane — asserted, not eyeballed. A second case slides
  *across a real crease* (an ACTIVE edge) and asserts the crease still
  deflects — the filter must not sand off real geometry.
- **No-tunnel.** Fountain onto the mesh floor: signed distance to the walked
  surface ≥ −ε for every live particle every frame; CCD bullet vs the mesh.
- **Raycast parity + determinism.** BVH raycast ≡ brute-force nearest hit;
  byte-identical candidate lists and manifolds across runs.
- **Scale (logged, not walled).** Midphase cost vs triangle count on a 100k+
  triangle mesh — the ~log curve is the reason the layer exists.
- **Loud guards for the 3am engineer.** Cooking reports (not swallows):
  degenerate-triangle count, open-edge count, weld statistics. Runtime guards:
  per-body candidate-count explosion (mistuned scene smell), and a
  non-manifold-contact guard when the internal-edge filter rejects every
  normal of a contact (a cooked-data-vs-runtime mismatch that must scream).

**Debug bus.** Two new bits appended to `AQDebug.h` (next free: `1U<<20`,
`1U<<21`): `AQDebugMeshMidphase` (outline the candidate triangles each body
touched this frame) and `AQDebugMeshEdge` (draw ACTIVE edges — the cooked
crease map a human can eyeball against the art). Same drainable bus, no new
transport.

---

## 10. Public API additions

```cpp
// AQSpace (additions) — the shape-factory idiom, one new named ctor:
/// Cook `desc` into a static triangle-mesh collider owned by this space and
/// return its shape handle. Cooking is deterministic; malformed input (null
/// arrays, indexCount % 3 != 0, all-degenerate) returns an invalid handle
/// LOUDLY. Mesh shapes are STATIC-ONLY: addBody rejects a dynamic body
/// wearing one (invalid body, stderr names the reason) — convex-decompose
/// dynamic props instead (the simple/complex rule as a contract).
AQShapeHandle createTriangleMeshShape(const AQMeshDesc &desc);   // new
```

Everything else is *existing surface growing a case*: `AQshapeAABB` (root AABB
from the directory record), `AQshapeSignedDistance` (closest-triangle query —
replacing today's implicit no-contact for unknown types), `raycast` /
`shapecast` / `overlap` mesh cases, and the narrowphase branch table. No new
handle types, no new step hooks, no backend types in `include/aqua/`.

---

## 11. Open decisions for this phase

1. **Midphase structure & node encoding — binary full-precision BVH vs
   quantized 4-wide (BVH34-like) vs grid-over-triangles.** *Lean: binary
   full-precision first* (simplest correct thing; the §9 oracles pin behavior
   before optimization), cooked via PLOC/binned-SAH with fixed tie-breaking;
   the quantized wide-node layout (Ylitie 2017 lineage) is the recorded GPU
   upgrade behind the same `AQMeshBVHNode` directory indirection. A
   grid-over-triangles was considered for symmetry with Phase 2 and rejected:
   triangle size variance within one mesh is exactly the grid's weakness.
2. **Broadphase integration — static world set vs grid insertion.** *Lean:
   world set* (§6.B): meshes are few, their AABBs are huge, and the grid's
   cell-size completeness bound must not inherit them. Revisit only if scenes
   with *many small* meshes appear (then: insert per-mesh root AABBs into the
   grid as ordinary entries).
3. **Internal-edge handling — cooked active-edge bits + normal clamping vs
   runtime normal welding vs nothing.** *Lean: cooked bits + clamping* (the
   Bullet lineage, §6.D) — it is the only variant that fixes flat floors
   without sanding off real creases, and it is cheap because the
   classification is paid at cook time.
4. **Particle/point query semantics — one-sided surface push-out vs
   winding-number signed distance.** *Lean: one-sided* (distance to nearest
   triangle, push along its front side): works for open meshes (terrain,
   walls), matches the one-sided narrowphase, and needs no solid/closed
   requirement on art. Cost, stated plainly: a fast particle can pass a thin
   open mesh between sub-steps (the analytic plane cannot tunnel; a mesh
   can). Mitigation is the engine's small-substeps posture + CCD for bodies;
   flag winding-number signed distance as the Phase 10 (fluids-in-containers)
   revisit, where solid meshes are natural.
5. **Static-strict vs kinematic meshes.** *Lean: static-strict now* — the
   collider snapshot and cooked inverse transform assume immobility, and
   kREATE's level geometry is static. Kinematic (translating platforms)
   flagged: it is a per-frame transform update, not a re-cook, so the door
   stays open cheaply.
6. **Heightfield — special type or mesh special case.** *Lean: separate
   follow-on type.* A regular-grid heightfield needs no BVH (cell lookup is
   O(1)) and deserves its cheaper path; forcing it through the mesh pipeline
   wastes exactly what makes it attractive. Mesh lands first and proves the
   consumer interfaces; heightfield reuses them.
7. **Cooking tolerances & residence — in-engine deterministic cook at create
   vs offline asset pipeline.** *Lean: in-engine* (kREATE hands raw
   vertices/indices; AQUA cooks deterministically). An offline cook (with the
   cooked pools as the serialized artifact) is the recorded follow-on once
   kREATE's asset pipeline wants to pay cook time at build instead of load;
   the cooked-pool layout is the serialization format either way. Weld
   tolerance and crease angle stay per-mesh `AQMeshDesc` knobs with stated
   defaults, not global config.
8. **Mesh-vs-mesh — permanently out.** Static-static pairs are never
   simulated, and dynamic bodies cannot wear meshes (§10); recorded so nobody
   "completes" it later by accident.

---

## 12. Recency-principle audit (written with the proposal, 2026-07-07)

Per roadmap §4, swept before proposing rather than retrofitted. *Honesty
note:* this is a desk audit from known literature and engine documentation;
the citations below should be re-verified (the Phase 7 §13.0 vet discipline)
when the phase is greenlit.

- **RT-core BVH traversal (Wang et al. arXiv 2409.09918, 2024; Mochi, arXiv
  2402.14801, 2024) — flagged, hardware-gated; NOTE: this phase is where it
  would bite hardest.** Phase 2 §12 deferred RT-core acceleration because the
  grid broadphase pays no BVH traversal. The mesh *midphase is* BVH traversal
  — the exact workload RT cores execute in silicon. The stance is unchanged
  (NVIDIA-specific vs the all-three-backends contract; software path
  required), but the flag is upgraded: when OmegaGTE exposes an RT feature
  gate, the mesh midphase is the first consumer to prototype against it.
- **PLOC++ (2022) / H-PLOC (Benthin et al., HPG 2024) — adopt-now for the
  cooking build.** The Phase 2 §12 citation update carries over; H-PLOC
  extends the line with a hierarchical variant claiming faster builds at
  equal quality (verify at implementation). Cooking is offline-ish (create
  time), so build *quality* (traversal cost) outweighs build speed — another
  reason PLOC-family over Karras.
- **Compressed wide BVHs (Ylitie 2017; PhysX BVH34 practice) — flagged for
  the GPU port, not the first cut.** Quantization is a bandwidth
  optimization; correctness and the §9 oracles come first on full-precision
  binary nodes. The directory indirection (§7) is shaped so the node format
  can swap without touching consumers.
- **SDF mesh collision (PhysX 5's deformable-collision line, Macklin et al.)
  — surveyed, rejected as primary, recorded as the dynamic-mesh answer.** See
  §6's alternative. AQUA's Phase 6 SDF vocabulary makes an SDF-collider
  variant natural *if Phase 9 ever needs dynamic mesh colliders*; for the
  static world, the BVH + exact triangles keep the no-snag and no-tunnel
  guarantees the voxel filter would soften.
- **Convex decomposition (V-HACD lineage; CoACD, SIGGRAPH 2022) — out of
  scope, recorded as the companion tool.** The static-only contract (§10)
  pushes dynamic props toward convex decomposition; CoACD is the current
  reference if kREATE later wants an in-pipeline decomposer feeding
  `createConvexHullShape`. An authoring-tool concern, not an AQUA runtime
  one.
- **Primitive tests (Möller–Trumbore 1997; Akenine-Möller 2001; Ericson
  2004) — no-divergence.** Nothing newer displaces the closed forms; modern
  engines still ship them verbatim.

**Net.** Classical answers hold everywhere they are settled (primitive tests,
AABB trees, cooked adjacency); the two modern adoptions are PLOC-family
cooking (already Phase 2 §12 doctrine) and the SoA/quantization-ready node
layout; the two recorded futures are RT-core midphase (hardware-gated — the
strongest future candidate in the whole collision stack) and SDF colliders
(dynamic meshes only). **Re-audit at greenlight** — especially H-PLOC
verification and any RT-core feature-gate movement in OmegaGTE.

---

*Brief status: **proposal (future work, unscheduled)** — the Phase 2 §11.6
deferral made concrete, written after Phase 6/7's GPU sub-phases landed so the
GPU-designed-for claims reference the live substrate that now exists. Settle
§11.1 (midphase structure), §11.3 (internal-edge handling), and §11.4
(point-query semantics) before implementation; the §13 implementation
contract gets written against ground-truth source at greenlight, per the
Phase 6/7 discipline. Natural scheduling window: before Phase 8 (cloth needs
level geometry to collide with), i.e. alongside the already-scheduled
pre-Phase-8 re-audit.*
