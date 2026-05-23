# Omega kREATE — Roadmap to a Complete 3D Game Engine

**Omega kREATE** (kREATE for short) is the project's 3D game engine.

This document describes, end to end, what it will take to grow kREATE from where it
is today into a complete 3D game engine. It is deliberately incremental: every
phase below is meant to land as a **runnable milestone**, not a big-bang rewrite.
We expect this to play out over many months, and the ordering here is a proposal,
not a contract — phases can be resequenced as priorities and findings change.

kREATE is a thin engine layer that sits on top of **OmegaGTE** (graphics/compute)
and **OmegaSL** (shading language); the editor will be built on **OmegaWTK**.
A guiding rule throughout: *if GTE already does it, kREATE wraps it — it does not
reimplement it.*

---

## 1. Where kREATE is today

What exists and works:

- **Window** — a native window on Win32, Cocoa, UIKit, Android, and X11. It can
  hand its surface to GTE as a render target. It currently only polls for
  *close* — there is no input, resize, or focus event surface yet.
- **App** — owns the GTE stack through an internal `Renderer`, drives an
  `onInit` / `onFrame` loop, and builds pipelines from OmegaSL source or a
  pre-compiled library.
- **Renderer (internal)** — owns the GTE device, command queue, and native
  render target. It can `beginFrame(clearColor)` and `endFrameAndPresent()`.
  It has no `draw()` yet.
- **Scene** — a flat scene graph with hierarchical world-transform propagation,
  camera view/projection matrices, and a clear color. **The per-object draw loop
  is commented out** — it is waiting on meshes being wired through.
- **Object** — a renderable node: transform, visibility, name, and a pipeline
  handle. **It has no mesh.**
- **Pipeline** — compiles OmegaSL (at runtime or from a `.omegasllib`) and builds
  a GTE render pipeline state, with cull/fill/depth options.
- **Math** — `Vec3`, `Vec4`, `Color`, and a column-major `Mat4` with
  perspective/lookAt/translation/rotation/scale. This is the *minimum* a camera
  needs and nothing more.

In one sentence: **kREATE can open a window and clear it to a color, and has the
skeleton of a scene graph and a pipeline system, but nothing is drawn yet.**

### What OmegaGTE already gives us to build on

We are not starting from the GPU up. GTE already provides, as backend-agnostic
APIs we can call from kREATE's internals:

- **Meshes** — `GEMesh` (GPU vertex/index buffers + texture bindings) and
  `GEMeshAsset` (import from OBJ, glTF 2.0, USD, Alembic).
- **Textures** — `GETexture` and `GETextureAsset` (KTX and friends).
- **Pipelines & shaders** — render/compute pipeline state, the OmegaSL compiler,
  mesh shaders.
- **Command submission** — typed command queues, command buffers, render passes.
- **Advanced GPU features** — raytracing, compute, a triangulation engine.

The roadmap below is mostly about **what kREATE must add on top** of these to be a
game engine rather than a graphics library: the asset/scene/gameplay/runtime
layers that turn "draw a mesh" into "ship a game."

---

## 2. What "complete" means — subsystem inventory

A complete 3D game engine is the union of these subsystems. kREATE has the first
two partially and the rest not at all.

| Subsystem | Today | Target |
|---|---|---|
| Windowing & platform | Window + close only | Full input, resize, DPI, fullscreen, lifecycle |
| Rendering | Clear + present | Meshes, materials, lighting, shadows, post, culling, transparency, instancing |
| Math | Minimal (Mat4 + vecs) | Quaternions, AABB/sphere, frustum, intersection, transforms |
| Camera | View/proj matrices on Scene | Camera objects + controllers, projection helpers |
| Asset pipeline | None | Import, cook, cache, hot-reload, package, stream |
| Scene / entity model | Flat transform graph | Entity-component model, prefabs, serialization, spatial queries |
| Resource management | Raw `shared_ptr` | Typed handles, lifetime, GPU residency, streaming |
| Input | None | Keyboard/mouse/gamepad/touch, action mapping |
| Time & frame loop | Bare `while` loop | Fixed/variable timestep, frame pacing, pause/scale |
| Physics | None | Collision, rigid bodies, character controller, raycast, triggers |
| Animation | None | Skeletal animation, skinning, blend trees, morph targets, tweening |
| Audio | None | 3D spatial audio, mixing, streaming, effects |
| Gameplay / scripting | C++ subclass of `App` | Behavior/component scripting, event bus, messaging |
| In-game UI | None | HUD/UI rendering, layout, input routing |
| Editor | Commented-out stub | Scene/asset/inspector tools on OmegaWTK, play-in-editor |
| Concurrency | Single-threaded | Job system, parallel frame graph |
| Tooling & debug | `std::cerr` | Logging, profiling, debug draw, console, validation |
| Networking *(optional)* | None | Replication, client/server, prediction |
| Packaging / distribution | Per-platform target scaffolding | Cooked builds, bundles, installers per platform |

---

## 3. Guiding principles

1. **Every phase ships something runnable.** No phase is "infrastructure only."
   Each ends with a `tests/` game (or editor scene) that demonstrates the new
   capability on screen.
2. **Wrap GTE; don't leak it.** GTE/OmegaSL types stay out of `include/kreate/*`.
   The header-isolation discipline established in
   `Pipeline-Object-Scene-Plan.md` is non-negotiable as we grow.
3. **Thin slice first, breadth later.** Get one mesh, one material, one light,
   one rigid body working end-to-end before generalizing.
4. **Decisions are surfaced, not buried.** The big architectural forks
   (entity model, rendering path, physics, scripting) are called out as **Key
   decisions** below. They should be made deliberately, not by whichever code
   gets written first.
5. **Author for the 3am on-call engineer.** Loud failures, useful logs, no
   silent default-returns in the runtime path.

---

## 4. Phased roadmap

Each phase lists its **goal**, the **runnable deliverable** that proves it, the
**work**, what it **depends on**, and any **key decisions** to make first.

### Phase 0 — Foundation *(done)*

Window + GTE render target + clear/present loop, with GTE fully isolated behind
kREATE's public headers. This is the current state.
*(See `docs/done/Vertical-Slice-Window-RenderTarget.md`.)*

---

### Phase 1 — First mesh on screen

**Goal:** Turn the commented-out draw loop in `Scene::render` into a real one.
Get a single hand-built mesh rendering through a user pipeline.

**Deliverable:** A game that draws a spinning colored triangle/cube using an
OmegaSL shader and a transform that updates per frame.

**Work:**
- Add a `Mesh` handle to kREATE's public surface, wrapping `GEMesh` internally.
- Add `Renderer::draw(pipeline, mesh, mvp)` — bind pipeline state, vertex/index
  buffers, push the MVP, issue the draw.
- Wire `Object` to hold a mesh; uncomment and implement the per-object draw loop
  in `Scene::render` (sort by pipeline, compute MVP, draw visible objects).
- Provide a way to build a mesh from CPU vertex/index data (a `MeshBuilder` or
  `Mesh::create(vertices, indices, layout)`).
- Establish the vertex-attribute contract between kREATE meshes and OmegaSL shaders
  (mirror `GEMeshVertexAttribute` ordering).

**Depends on:** Phase 0. GTE `GEMesh` already exists.

**Key decision:** How do per-object/per-frame uniforms (MVP, etc.) get to the
shader — push constants, a per-draw uniform buffer, or a bindless scheme? This
sets the shape of the material system later.

---

### Phase 2 — Math & camera foundation

**Goal:** Grow `Math` from "enough for one lookAt" into a real game-math library,
and introduce first-class cameras.

**Deliverable:** A free-fly camera the user can move/rotate (once Phase 5 input
lands; until then, scripted motion), with correct quaternion-based rotation.

**Work:**
- `Quat` (quaternions): construction, slerp, to/from matrix and Euler.
- `Vec2`/`Mat3`, vector ops (dot/cross/normalize/length), matrix inverse/transpose.
- Bounding volumes: `AABB`, `Sphere`; a `Frustum` extracted from view-proj.
- Intersection tests (ray/AABB, ray/sphere, ray/triangle, frustum/AABB).
- A `Transform` type (position + rotation + scale) that composes to a `Mat4`.
- `Camera` object: perspective/orthographic, exposes view/projection; replaces
  raw matrix setters on `Scene` (which can keep them as a convenience).

**Depends on:** Phase 1 (to have something to point a camera at).

---

### Phase 3 — Asset pipeline I: import models & textures

**Goal:** Load real 3D models and textures from disk instead of hand-coding
vertices.

**Deliverable:** A game that loads a glTF model with its base-color texture and
renders it.

**Work:**
- `ModelAsset` / `Mesh::load(path)` wrapping `GEMeshAsset` (OBJ/glTF/USD).
- `Texture` / `TextureAsset` wrapping `GETexture` / `GETextureAsset` (KTX, etc.).
- A path/resolution convention for asset files relative to the game bundle.
- Synchronous load first; async (`loadAsync`) exposed once the job system exists.
- Surface materials/texture bindings that arrive with the imported mesh.

**Depends on:** Phase 1. GTE `GEMeshAsset`/`GETextureAsset` already exist.

**Key decision:** Do we adopt an **asset database / cooking** model now (stable
IDs, an import-once/cache step, hot-reload) or stay file-path-direct and add the
database later (Phase 14)? Cheaper to decide the ID scheme early.

---

### Phase 4 — Materials & lighting

**Goal:** Move from "a pipeline per object" to a **material** abstraction, and
light the scene.

**Deliverable:** A model rendered with a PBR-style material under a directional
light plus ambient, with correct normals.

**Work:**
- `Material`: a shader (pipeline) + named parameter set (scalars, vectors,
  textures) + render state. Objects reference a material, not a raw pipeline.
- A standard surface shader (OmegaSL) and a parameter-binding path
  (per-material/per-object uniform buffers).
- `Light` types: directional, point, spot — with color/intensity/range.
- A lighting model in the standard shader (start with a single light, then
  multiple); decide forward vs. deferred (see decision).
- Normal/tangent handling and a normal-mapping path.
- Camera/scene uniforms (view pos, light list) delivered to shaders.

**Depends on:** Phases 2, 3.

**Key decision:** **Forward vs. deferred vs. forward+ rendering.** This shapes
the render loop, the G-buffer (or lack of one), shadow integration, and
transparency handling. Pick based on target platforms and light counts.

---

### Phase 5 — Input & time

**Goal:** Make the engine interactive and give it a real frame clock.

**Deliverable:** The Phase 2 camera flown around the Phase 4 scene with
keyboard/mouse, frame-rate-independent.

**Work:**
- Extend `Window` to emit input events: keyboard, mouse (move/buttons/wheel),
  and per-platform plumbing (Win32, Cocoa, UIKit, Android touch, X11).
- An `Input` system: polled state + an event queue; an **action-mapping** layer
  (bind "MoveForward" to keys/axes) so gameplay code is device-agnostic.
- Gamepad and touch input.
- A `Time`/clock: delta time, total time, fixed-timestep accumulator for physics,
  pause and time-scale.
- Resize/DPI/focus handling: rebuild the render target and update projection on
  resize.

**Depends on:** Phase 0 (window) and benefits from Phase 2 (camera).

---

### Phase 6 — Scene model & serialization

**Goal:** Decide and build kREATE's core **entity/component model**, and make
scenes saveable/loadable.

**Deliverable:** A scene authored as data (file), loaded at runtime, with
entities composed from components (transform, mesh-renderer, light, camera).

**Work:**
- The entity-component model (see decision) replacing/extending today's flat
  `Object` graph: entities, components, systems or behaviors.
- Built-in components: Transform, MeshRenderer, Light, Camera.
- **Prefabs** (reusable entity templates) and instantiation.
- **Scene serialization**: save/load to a human-diffable text format; stable
  references between entities and to assets.
- Spatial structure for culling/queries (grid/BVH/octree) feeding the renderer's
  frustum culling.

**Depends on:** Phases 1–5. This is the backbone the rest of the engine hangs on.

**Key decision (foundational):** **Entity model — ECS (data-oriented, archetypes/
systems) vs. an object/component tree (OOP, components own behavior).** This is
the single most far-reaching decision in the roadmap. It affects performance,
the editor, scripting, serialization, and every subsystem after it. Decide before
building Phase 6, not during.

---

### Phase 7 — Rendering II: shadows, transparency, post, render graph

**Goal:** Production-grade rendering.

**Deliverable:** The scene with real-time shadows, a skybox/IBL, transparent
objects sorted correctly, and at least one post-processing effect (tone-mapping
or bloom).

**Work:**
- **Shadow mapping** (directional first; cascades; then spot/point/cube).
- **Culling**: frustum culling from Phase 6's spatial structure; later occlusion.
- **Transparency**: sorted alpha blending; an ordered pass after opaque.
- **Skybox / image-based lighting**: environment maps, irradiance/prefilter,
  reflections.
- **Post-processing stack**: HDR, tone-mapping, bloom, FXAA/TAA, color grading.
- **Render graph / frame graph**: declare passes and resources, let the engine
  schedule/alias them. This becomes the renderer's backbone for everything above.
- **Instancing** and **LOD** hooks (full LOD/streaming in Phase 14).
- Optional, leveraging GTE: raytraced reflections/shadows/GI as an advanced path.

**Depends on:** Phases 4, 6.

---

### Phase 8 — Physics & collision

**Goal:** Things move, fall, and collide.

**Deliverable:** A scene with rigid bodies falling onto static geometry, a
character controller walking on it, and gameplay raycasts that hit.

**Work:**
- Collision shapes (box/sphere/capsule/mesh), broadphase + narrowphase.
- Rigid-body dynamics integrated against the fixed timestep from Phase 5.
- A character controller and kinematic bodies.
- Raycasts / shape-casts / overlap queries for gameplay.
- Triggers and collision callbacks routed to the gameplay layer (Phase 11).
- Components: Collider, RigidBody, CharacterController (Phase 6 model).

**Depends on:** Phases 5 (fixed timestep), 6 (components).

**Sibling module — AQUA.** Physics lives in its own module, **AQUA**, which is to
simulation what OmegaGTE is to graphics: a dedicated engine kREATE consumes rather
than embeds. kREATE's job here is the *integration* layer — collider/rigid-body
components, the fixed-timestep step, and routing collision/trigger callbacks into
the gameplay layer — over whatever AQUA exposes. (A third-party physics SDK is
currently vendored under `kreate/deps/`; whether AQUA wraps it or replaces it is
AQUA's own roadmap decision, kept out of kREATE's public surface the same way GTE
is.)

**Key decision:** The build-vs-integrate question now lives mostly in **AQUA**.
For kREATE, the decision is the **integration boundary**: what physics types cross
into kREATE's public API (none — keep them behind components, as with GTE) and how
deterministic the step must be (see Determinism under cross-cutting concerns).

---

### Phase 9 — Animation

**Goal:** Animate characters and objects.

**Deliverable:** A skinned character playing an imported animation clip, with two
clips blended.

**Work:**
- Skeletons and **skeletal animation** (clips imported via `GEMeshAsset`).
- **GPU/CPU skinning** integrated with the mesh/material path.
- Animation **blending**, blend trees, and state machines.
- Morph-target/blend-shape support.
- A general **tween/timeline** system for non-skeletal animation (transforms,
  material params).
- Components: Animator, SkinnedMeshRenderer.

**Depends on:** Phases 4 (materials/shaders), 6 (components), 3 (import).

---

### Phase 10 — Audio

**Goal:** The engine can hear and be heard.

**Deliverable:** A scene with a positioned 3D sound source that attenuates as the
camera moves, plus a music track.

**Work:**
- An audio backend abstraction (platform mixers) — likely a sibling library or a
  wrapped dependency, isolated like GTE.
- 3D **spatial audio**: listener, sources, attenuation, doppler.
- Mixing, buses, and streaming for long tracks.
- Effects (reverb, low-pass) and an occlusion hook.
- Components: AudioSource, AudioListener.

**Depends on:** Phases 5 (time), 6 (components).

**Key decision:** Build vs. integrate (same shape as physics).

---

### Phase 11 — Gameplay & scripting

**Goal:** Author game logic without recompiling the engine — or with a clean,
ergonomic C++ behavior layer.

**Deliverable:** A small playable demo (move, interact, win/lose) authored mostly
in the gameplay layer rather than by subclassing `App`.

**Work:**
- A **behavior/component scripting** model: lifecycle hooks
  (`onStart`/`onUpdate`/`onCollision`/…) attached to entities.
- An **event bus / messaging** system between components and systems.
- A scripting surface (see decision): native C++ behaviors, a hot-reloadable
  module boundary, or an embedded scripting language.
- Coroutines/timers for time-based gameplay.
- Save-game / game-state serialization built on Phase 6's serializer.

**Depends on:** Phases 5, 6, and ideally 8 (collision callbacks).

**Key decision:** **Scripting approach** — pure C++ behaviors, hot-reloaded
native modules, or an embedded language. Trades iteration speed, performance,
and tooling/editor complexity. Affects the editor (Phase 13) heavily.

---

### Phase 12 — In-game UI

**Goal:** HUDs, menus, and on-screen UI inside the game (distinct from the
OmegaWTK-based editor).

**Deliverable:** A main menu and an in-game HUD (health bar, score) that respond
to input.

**Work:**
- A UI rendering path (text, sprites, 9-slice) layered over the 3D frame.
- Layout (flex/anchors) and a widget set (button, label, image, slider).
- UI input routing from Phase 5, with focus and event capture.
- Data binding to gameplay state.

**Depends on:** Phases 5, 7. **Key decision:** reuse OmegaWTK for in-game UI, or
build a lightweight immediate/retained UI tuned for game frame rates.

---

### Phase 13 — Editor

**Goal:** Stand up the kREATE editor (the commented-out CMake stub) on OmegaWTK.

**Deliverable:** An editor that opens a scene, shows a viewport, lets you
select/move entities with gizmos, edit components in an inspector, browse assets,
and **play in editor**.

**Work:**
- Editor shell on OmegaWTK: docking, viewport, panels.
- **Scene hierarchy** + **inspector** (reflection over the component model).
- **Asset browser** wired to the asset pipeline (Phase 3/14).
- **Transform gizmos** and viewport picking (uses Phase 2 intersection math).
- **Play-in-editor**: run the runtime loop inside the editor; pause/step.
- Undo/redo over scene edits; serialization round-trips (Phase 6).

**Depends on:** Phases 6 (entity model + serialization), 11 (scripting model
drives the inspector), 7 (viewport rendering). This is large; it can begin in
parallel once Phase 6 stabilizes, growing alongside later phases.

**Key decision:** This phase depends on the **reflection** story for components —
how the inspector discovers and edits component fields. Decide the reflection
mechanism (macros, codegen, manual registration) when Phase 6 lands.

---

### Phase 14 — Asset pipeline II & world scale

**Goal:** Ship-quality content handling and large worlds.

**Deliverable:** A large scene that streams in/out around the camera at a stable
frame rate, built from a cooked asset package.

**Work:**
- **Asset cooking/baking**: import-time conversion to runtime-optimized formats
  (compressed textures, optimized meshes), with a content-addressed cache.
- **Hot-reload** of assets in editor and dev builds.
- **Streaming**: load/unload assets and scene regions by distance/visibility.
- **LOD**: mesh and material LOD selection.
- **Resource management**: typed handles, reference counting, GPU residency
  budgets, eviction.
- A **job system** (if not already introduced) to make all of the above async.

**Depends on:** Phases 3, 6, 7. The job system here also benefits earlier phases
retroactively (async load, parallel culling).

---

### Phase 15 — Networking *(optional for "complete")*

**Goal:** Multiplayer, if the project needs it.

**Deliverable:** Two clients seeing each other move in a shared scene.

**Work:** Transport layer, entity/state **replication**, client prediction +
server reconciliation, RPCs/events over the wire, authority model.

**Depends on:** Phases 6 (entity model), 11 (gameplay). **Key decision:** whether
networking is in scope at all, and if so, the topology (authoritative server,
peer-to-peer, lockstep) — it constrains the gameplay and physics layers, so
decide before Phase 11 if it's a goal.

---

### Phase 16 — Tooling, profiling & distribution

**Goal:** Make the engine debuggable in production and shippable on every target.

**Deliverable:** A profiled build packaged into a runnable bundle/installer on
each platform.

**Work:**
- **Logging** with levels/categories (replace `std::cerr` in the runtime path).
- **Profiling**: CPU/GPU timers, frame stats, an in-engine overlay.
- **Debug draw**: lines, shapes, text in world space (invaluable for physics/AI).
- **Developer console** and runtime tweakable variables.
- A **validation/debug layer** for the runtime (mirrors GTE's debug-layer work).
- **Packaging/distribution** per platform, building on the `target/` scaffolding:
  bundle layout, asset packaging, code signing, installers.

**Depends on:** runs alongside everything; mature it as subsystems land.

---

## 5. Cross-cutting concerns

These don't fit one phase — they thread through several and should be designed
early even if implemented gradually:

- **Concurrency / job system** — introduce by Phase 7–8 at the latest;
  retrofits async asset loading, culling, animation, and physics.
- **Memory management** — allocation strategy, GPU buffer pooling, frame
  allocators. Decide before the renderer and asset systems grow large.
- **Error handling philosophy** — loud, logged failures in the runtime; no silent
  default-returns. Establish the result/exception convention engine-wide.
- **Reflection** — needed for the editor inspector and serialization. The
  mechanism chosen in Phase 6 ripples into Phases 11 and 13.
- **Determinism** — if networking (lockstep) or replay is ever wanted, physics
  and gameplay must be deterministic; that constrains the math and physics layers.
- **Header isolation** — keep GTE/OmegaSL out of `include/kreate/*` as the surface
  grows (the discipline from `Pipeline-Object-Scene-Plan.md`).

---

## 6. Key decisions to make early

These are the forks that are expensive to reverse. They are pulled out of the
phases above so they can be decided deliberately:

1. **Entity model — ECS vs. object/component tree.** (Phase 6) The most
   far-reaching choice; everything downstream assumes it.
2. **Rendering path — forward / deferred / forward+.** (Phase 4) Shapes the
   render graph, shadows, transparency.
3. **Uniform/binding strategy — push constants / UBOs / bindless.** (Phase 1)
   Sets the material and per-draw data shape.
4. **Asset model — file-direct vs. cooked database with stable IDs.** (Phase 3)
   Cheaper to pick the ID scheme up front.
5. **Physics & audio integration boundary.** (Phases 8, 10) Physics is the
   **AQUA** sibling module (build-vs-integrate is AQUA's call); audio is likely a
   sibling too. For kREATE, decide what — if anything — of their types crosses into
   the public API (keep it behind components, as with GTE).
6. **Scripting — C++ behaviors / hot-reload native / embedded language.**
   (Phase 11) Drives iteration speed and editor complexity.
7. **In-game UI — reuse OmegaWTK vs. dedicated game UI.** (Phase 12)
8. **Networking in scope?** (Phase 15) If yes, decide the topology before
   Phase 11, because it constrains gameplay and physics.

---

## 7. Dependency overview

```
Phase 0  Window + clear              (done)
   │
Phase 1  First mesh on screen ───────────────┐
   │                                          │
Phase 2  Math & camera                        │
   │                                          │
Phase 3  Assets I (models, textures) ─────────┤
   │                                          │
Phase 4  Materials & lighting                 │
   │                                          │
Phase 5  Input & time                         │
   │                                          │
Phase 6  Entity model & serialization  ◄── foundational; gates the rest
   ├───────────────┬───────────────┬──────────┬───────────────┐
Phase 7         Phase 8         Phase 9     Phase 11        Phase 13
Rendering II    Physics         Animation   Gameplay/       Editor
(shadows,                                    scripting       (parallel,
 post, RG)         │                            │            grows w/ rest)
   │               │                            │
Phase 12  In-game UI  ◄── needs 5, 7        Phase 10  Audio
   │
Phase 14  Assets II & streaming  ◄── needs 3, 6, 7 (+ job system)
   │
Phase 15  Networking (optional)  ◄── needs 6, 11
   │
Phase 16  Tooling & distribution ◄── matures alongside everything
```

The critical path to "you can build a game in it" runs **Phase 1 → 6**, after
which several tracks (rendering, physics, animation, gameplay, editor) can
progress in parallel as resourcing allows.

---

*This roadmap is a living document. Phases will be split, merged, and reordered
as we learn. The intent is to keep kREATE shippable and demonstrable at every step
rather than gated on a distant "done."*
