# AQUA Game Engine: Design Document (Stub → Working Engine)

This document proposes an architecture to evolve AQUA from its current stub into a working game engine, respecting the constraints in [AGENTS.md](../AGENTS.md): Unity-like simplicity for authoring, Unreal-like 3D capability, OmegaGTE as the graphics layer, and OmegaWTK for the editor.

---

## 1. Current State Summary

### 1.1 What Exists (Stub)

| Component | Location | Status |
|-----------|----------|--------|
| **Core** | `AQUABase.h`, `AQUACore.h`, `AQUAObject.h/cpp` | Base types, `Object`, `Transform` (partial), property macros |
| **Scene** | `AQUAScene.h/cpp` | `Scene` (object container), `SceneRenderer` (empty interface) |
| **Frontend** | `AQUAFrontend.h` | `Frontend` with `launch()`, `loadScene()` |
| **Editor** | `EditorMain.cpp` | OmegaWTK entry point only (`omegaWTKMain` returns 0) |
| **Build** | `CMakeLists.txt` | AQUA static library, depends on OmegaGTE; editor not wired to AQUA |

### 1.2 Gaps and Inconsistencies

- **Transform**: Declared with `Transform() = delete` and a 6-parameter constructor that is never defined; only `Translate`/`Rotate` implemented.
- **Scene vs PhysObject**: `Scene::addObject` in `.cpp` takes `SharedHandle<PhysObject>`, but the header uses `SharedHandle<Object>` and `PhysObject` is commented out in `AQUAObject.h`.
- **SceneRenderer**: Purely declarative; no tie to OmegaGTE (no `GENativeRenderTarget`, command buffers, or render passes).
- **Frontend**: No implementation; no window creation, no main loop, no scene loading path.
- **Editor**: Does not link or use AQUA; no scene editing, no viewport, no asset or entity UI.

---

## 2. Design Principles

1. **Unity-style ergonomics**: Few core concepts (Scene, Entity/GameObject, Component, System), clear lifecycle (Awake/Start/Update), and simple asset/scene loading where possible.
2. **Unreal-style power**: Full 3D (world, transforms, cameras, lights), render layers, and room for advanced features (LOD, culling, later: blueprints/visual scripting).
3. **OmegaGTE as the only graphics backend**: All rendering goes through GTE (devices, command queues, render targets, pipelines, textures, buffers). No duplicate abstraction; AQUA is a *user* of GTE.
4. **OmegaWTK for editor only**: Game runtime is headless or uses a minimal window; the full editor (viewport, hierarchy, inspector) is an OmegaWTK app that drives AQUA scenes and assets.
5. **Modular, prefix, style**: All public types/files use `AQUA` prefix and PascalCase; code style LLVM/Google; logical modules (Core, Scene, Rendering, Physics, Audio, Scripting, EditorBridge).
6. **PhysX for physics**: Whenever possible, use **NVIDIA PhysX** for physics: rigid bodies, collision detection, shapes, scenes, and (where applicable) character controllers, cloth, or particles. Prefer PhysX over other physics libraries or custom implementations for consistency and production-grade behaviour.

---

## 3. Proposed Architecture

### 3.1 Layering

```
┌─────────────────────────────────────────────────────────────────┐
│  Editor (OmegaWTK)  │  Game Runtime (optional minimal window)   │
├─────────────────────────────────────────────────────────────────┤
│                    AQUA Engine                                   │
│  ┌──────────┬──────────┬──────────┬──────────┬──────────────┐  │
│  │ Core     │ Scene    │ Rendering│ Physics  │ Audio / Input │  │
│  │ (Object, │ (Scene,  │ (Renderer│ (PhysX)  │ (optional     │  │
│  │  App)    │  Entity) │  Camera) │          │  later)       │  │
│  └──────────┴──────────┴──────────┴──────────┴──────────────┘  │
├─────────────────────────────────────────────────────────────────┤
│  OmegaGTE (Graphics)  │  OmegaCommon  │  PhysX (NVIDIA)          │
└─────────────────────────────────────────────────────────────────┘
```

- **Core**: Application lifecycle, object model, reflection/metadata (optional), serialization hooks.
- **Scene**: Scene graph, entities, transforms, hierarchy; data only, no GPU.
- **Rendering**: Binds Scene + OmegaGTE: cameras, meshes, materials, lights → GTE command buffers and render targets.
- **Physics**: PhysX (NVIDIA) for rigid bodies, collision, and scene simulation; AQUA physics components sync entity transforms with PhysX actors. Audio and Input remain placeholder or thin wrappers until needed.

### 3.2 Core Module

**Goals**: Stable object model, clear ownership, and a single application entry point that can be driven by the editor or by a standalone game.

| Concept | Description |
|---------|-------------|
| **AQUAObject** | Keep as base for engine-owned objects. Fix/replace with **AQUAGameObject** (or keep `Object` as internal base and expose “Entity” in the scene API). |
| **AQUATransform** | Full 3D transform: position (x,y,z), rotation (pitch, yaw, roll or quaternion), scale (sx,sy,sz). Implement constructor and optionally a 4×4 matrix or use a small math type; ensure it’s usable from both scene and renderer. |
| **AQUAApplication** | Singleton or explicit instance: `Init()`, `Run()` (or step), `Shutdown()`. Holds default GTE device/engine and optional main window. Frontend becomes the driver that creates AQUAApplication and calls Run. |

**Files (new/renamed)**:

- `AQUATransform.h/cpp` – Complete transform API and implementation (including constructor).
- `AQUAApplication.h/cpp` – Engine init, main loop hook, shutdown; owns `GTE` (from OmegaGTE) and optionally a main window handle for the runtime.
- Resolve **Object** vs **PhysObject**: Either uncomment and fix PhysObject as “scene entity with transform” or introduce **AQUAEntity** that has a Transform and is the type stored in `Scene::objectContainer`. Prefer one clear type (e.g. Entity) for “thing in the scene” to keep Unity-like simplicity.

**Naming**: Keep `AQUABase.h` for macros and shared types; keep `AQUAObject.h` for the base Object if it remains the root of the hierarchy; add `AQUAEntity.h` if you want a clear scene-node type.

### 3.3 Scene Module

**Goals**: A clear scene graph: one current scene, entities with transforms and parent/child relationship, and a place to attach “components” (renderable, light, etc.) without implementing full ECS yet.

| Concept | Description |
|---------|-------------|
| **Scene** | Container of entities and scene-level config (dimensions, environment). `addObject`/`addEntity` should take the chosen entity type; fix the current Object/PhysObject mismatch. |
| **Entity (or GameObject)** | Has a unique ID, a Transform, optional parent, list of children. Holds a list of **Components** (pointers or type-erased). |
| **Component** | Interface or base class: e.g. `AQUAMeshComponent`, `AQUACameraComponent`, `AQUALightComponent`. Scene traverses entities and their components for the renderer. |

**Files**:

- `AQUAScene.h/cpp` – Extend Scene with `load()`/`unload()` hooks, and optionally root entity list and name/index lookup.
- `AQUAEntity.h/cpp` – Entity with Transform, parent/child, and component list (or slot for one render component + one light, etc., to start simple).
- `AQUAComponent.h` – Base component type; optional `AQUAMeshComponent.h`, `AQUACameraComponent.h`, `AQUALightComponent.h` as first concrete components.

**SceneRenderer** (see 3.4) will *read* from Scene + Entities + Components and drive OmegaGTE; it does not own the scene graph.

### 3.4 Rendering Module (AQUA → OmegaGTE)

**Goals**: One place that turns the current Scene into GTE commands: acquire swapchain/render target from the window, run a single (or few) render passes, and present. No duplicate concepts (e.g. “material” in AQUA should map to GTE pipeline state + resources, not a parallel abstraction).

| Concept | Description |
|---------|-------------|
| **AQUARenderer** | Replaces or implements current `SceneRenderer`. Holds: `SharedHandle<OmegaGraphicsEngine>`, `SharedHandle<GENativeRenderTarget>` (or equivalent for the main window), and possibly a default `GECommandQueue`. On `render(Scene const&)` (or per-frame callback): clear, iterate cameras, draw meshes that have a mesh component and material, then present. |
| **AQUACamera** | Data only (FOV, near/far, viewport). Can live as a component on an Entity; renderer uses it to set view/projection and cull. |
| **AQUAMesh / AQUAMaterial** | Thin wrappers or IDs: AQUAMesh holds a GTE buffer (or reference to vertex/index data) and topology; AQUAMaterial holds or references a `GERenderPipelineState` and bindings (textures, constants). Prefer “resource handle” style (AQUAMeshId, AQUAMaterialId) backed by a manager that owns GTE resources. |
| **Lights** | Optional: directional/point/spot data on a component; renderer passes to a simple forward pass or a small uniform buffer. Can be stub (single directional) at first. |

**Flow**:

1. **Frontend** or **AQUAApplication** creates the main window (or gets a native handle from OmegaWTK in editor).
2. Create `GTE` (Init/InitWithDefaultDevice), then create `GENativeRenderTarget` from the window.
3. Each frame: `AQUARenderer::beginFrame()` → get command buffer, clear; for each camera, `drawScene(camera, scene)` (cull, set pipeline, bind mesh + material, draw); `endFrame()` / present.

**Files**:

- `AQUARenderer.h/cpp` – Owns GTE render target and command queue; implements begin/end frame and draw-scene loop.
- `AQUACamera.h` – Camera parameters (and optionally view/projection matrix helper).
- `AQUAMesh.h/cpp`, `AQUAMaterial.h/cpp` or `AQUARenderResources.h/cpp` – Mesh and material as GTE-backed handles; can start with a single default pipeline and one test mesh (e.g. cube or quad from OmegaGTE tessellation or hand-built).

**Dependency**: Rendering module depends on OmegaGTE and on Scene/Core; Scene must not depend on Rendering (one-way dependency).

### 3.5 Physics Module (PhysX)

**Goals**: Use NVIDIA PhysX for all physics-based behaviour whenever possible: rigid-body dynamics, collision shapes, scene simulation, and (optionally) character controllers, cloth, or particle systems. Keep a thin AQUA wrapper so the rest of the engine stays PhysX-agnostic.

| Concept | Description |
|---------|-------------|
| **AQUAPhysicsScene** | Wraps a PhysX `PxScene`. Created per AQUA Scene (or one global); stepped each frame with a fixed or variable timestep. Owns PhysX foundation and physics SDK instance if not shared. |
| **AQUARigidBody** (or **AQUAPhysicsBody**) | Component or resource that wraps a PhysX rigid body (`PxRigidDynamic` / `PxRigidStatic`). Syncs to/from entity Transform: AQUA → PhysX for kinematic updates; PhysX → AQUA after simulation step for dynamic bodies. |
| **AQUACollisionShape** | Thin wrapper over PhysX shapes: box, sphere, capsule, convex mesh, triangle mesh. Attached to rigid bodies; used for collision and queries. |
| **Sync order** | Each frame: apply kinematic/script-driven transform changes into PhysX → step scene → read back positions/rotations for dynamic bodies into entity Transforms so the renderer sees updated poses. |

**Files**:

- `AQUAPhysicsScene.h/cpp` – Create/destroy PhysX scene, step simulation, optional gravity and solver config.
- `AQUARigidBody.h/cpp` or `AQUAPhysicsBody.h/cpp` – Component that holds a PhysX actor; sync with Transform; mass, damping, constraints (later).
- `AQUACollisionShape.h/cpp` – Shape creation (box, sphere, capsule, etc.) and attachment to rigid bodies.
- Optional: `AQUAPhysicsWorld.h/cpp` – If you prefer a single global physics world that can host multiple AQUA scenes or sub-scenes.

**Dependency**: Physics module depends on PhysX SDK (linked statically or dynamically) and on Core/Scene for Transform and Entity. Rendering and other systems only see transforms; they do not depend on PhysX.

**Build**: Add PhysX as a dependency (e.g. via CMake `find_package(PhysX)` or a vendored/submodule build). Make the Physics module optional so headless or minimal builds can exclude it if needed.

### 3.6 Frontend and Application Lifecycle

**Goals**: A single path to “run the engine” (game or editor-hosted), with clear ownership of GTE and the main window.

| Concept | Description |
|---------|-------------|
| **Frontend** | High-level entry: parse args, create AQUAApplication, load initial scene (or open project in editor), then run. `launch()` becomes: init GTE, create window (if runtime), create AQUARenderer, load initial scene, enter main loop. |
| **Main loop** | Fixed or variable timestep: input (if any) → **physics step** (PhysX scene step, then sync transforms) → scene update (scripts later) → render (AQUARenderer::render) → present. Can be driven by OmegaWTK in editor (WTK’s run loop calls AQUA tick + render). |

**Files**:

- `AQUAFrontend.cpp` – Implements `launch()`, `loadScene()`; wires AQUAApplication + AQUARenderer + Scene.
- `AQUAApplication.cpp` – Already proposed in Core; provides `Run()` or `Tick()` so the editor can drive the engine step-by-step.

### 3.7 Editor (OmegaWTK)

**Goals**: Editor is an OmegaWTK app that uses AQUA as a library: load/save scenes, show hierarchy and properties, and render the current scene into a viewport.

| Concept | Description |
|---------|-------------|
| **Viewport** | One or more widgets that hold a render target (e.g. offscreen GTE texture or native surface). Each frame, call AQUARenderer with the viewport’s target and the current scene. |
| **Hierarchy** | List of entities in the current scene; selection and parent/child editing. |
| **Inspector** | Properties of selected entity (transform, components); edit and push changes back into AQUA entities. |
| **Scene load/save** | Serialize Scene + entities + components to disk; deserialize into AQUA structures. Format can be JSON or a simple binary for v1. |

**Files** (under `editor/` or a new `editor/AQUAEditor/`):

- Wire editor CMake to link AQUA and OmegaGTE; ensure one GTE device is shared or created for the viewport.
- `EditorMain.cpp` – Create main window, menu, viewport panel, hierarchy panel; on tick, call AQUAApplication::Tick() and AQUARenderer for the viewport.
- Optional: `AQUASceneSerialization.h/cpp` in engine for load/save so both game and editor use the same code.

### 3.8 Stub Fixes (Immediate)

Before or in parallel with the above:

1. **Transform**: Add `Transform::Transform(float x, float y, float z, float pitch, float yaw, float roll)` in `AQUAObject.cpp` (or in a dedicated AQUATransform.cpp). Consider allowing default constructor (e.g. identity) for easier use in containers.
2. **Scene::addObject**: Change signature to use one type consistently. Either:
   - Use `SharedHandle<Object>` and add a concrete subclass (e.g. Entity) that has a Transform and is what the scene actually stores, or
   - Uncomment PhysObject, add a base Object with minimal data, and make PhysObject the “scene entity” with Transform; then fix the header to match the .cpp.
3. **SceneRenderer**: Rename or replace with AQUARenderer and move it into the new Rendering module with a clear dependency on OmegaGTE headers and implementation.

---

## 4. Phased Implementation Plan

### Phase 1: Core and scene consistency (no new rendering yet)

- Fix Transform (constructor, optional default).
- Resolve Object/Entity/PhysObject: one canonical “scene node” type and use it in Scene.
- Add AQUAApplication (init, run/tick, shutdown) and optionally AQUAEntity + AQUAComponent base.
- Get the editor linking AQUA; from editor main, create AQUAApplication and one empty Scene; no viewport yet.

### Phase 2: First frame with OmegaGTE

- Add AQUARenderer: create GTE and GENativeRenderTarget from a window (can be a test window in a small runtime main, or a WTK viewport).
- Add AQUACamera and one AQUAMesh (e.g. fullscreen quad or cube); add AQUAMaterial (one pipeline state).
- Implement draw path: one camera, one mesh, one pass; clear and present. Prove that AQUA can drive OmegaGTE end-to-end.

### Phase 3: Scene-driven rendering

- Connect Scene to Renderer: traverse entities, collect mesh and camera components, cull (trivially at first), and draw. One scene, one camera, multiple meshes.
- Optional: simple lights and a second pass or uniform for lighting.

### Phase 4: Editor integration

- Viewport in OmegaWTK that uses AQUARenderer to draw the current scene.
- Hierarchy and inspector for entities and transform (and later components).
- Scene load/save (serialization) and basic project/scene workflow.

### Phase 5: Physics (PhysX)

- Add PhysX SDK as a dependency (CMake, optional or required).
- Implement AQUAPhysicsScene (create/destroy PxScene, step simulation).
- Add AQUARigidBody (and optionally AQUACollisionShape) component; sync entity Transform ↔ PhysX actor each frame.
- Integrate physics step into main loop (before or after scene update); ensure dynamic bodies drive rendered poses.

### Phase 6: Polish and extension

- Input module (keyboard/mouse/gamepad) and optional scripting hook.
- Extend PhysX usage where applicable: character controller, triggers, raycasts, cloth/particles if needed.
- Audio placeholder or integration.
- Documentation and samples (minimal “blank scene” and “cube + camera” demos).

---

## 5. File and Module Layout (Proposed)

```
aqua/
├── AGENTS.md
├── docs/
│   └── AQUA-Design-Stub-to-Engine.md   (this file)
├── CMakeLists.txt
├── engine/
│   ├── include/
│   │   ├── AQUA.h
│   │   ├── AQUAFrontend.h
│   │   └── aqua/
│   │       ├── Core/
│   │       │   ├── AQUABase.h
│   │       │   ├── AQUACore.h
│   │       │   ├── AQUAObject.h
│   │       │   ├── AQUATransform.h      (new or moved)
│   │       │   ├── AQUAApplication.h    (new)
│   │       │   └── AQUAEntity.h         (new, or merge into Object)
│   │       ├── Scene/
│   │       │   ├── AQUAScene.h
│   │       │   ├── AQUAComponent.h      (new)
│   │       │   └── ...
│   │       ├── Rendering/
│   │       │   ├── AQUARenderer.h       (new)
│   │       │   ├── AQUACamera.h         (new)
│   │       │   ├── AQUAMesh.h           (new)
│   │       │   └── AQUAMaterial.h       (new)
│   │       └── Physics/
│   │           ├── AQUAPhysicsScene.h   (new, PhysX)
│   │           ├── AQUARigidBody.h      (new, PhysX)
│   │           └── AQUACollisionShape.h (new, PhysX)
│   └── src/
│       ├── Core/
│       │   ├── AQUAObject.cpp
│       │   ├── AQUATransform.cpp        (new or extended)
│       │   ├── AQUAApplication.cpp      (new)
│       │   └── AQUAEntity.cpp           (new)
│       ├── Scene/
│       │   ├── AQUAScene.cpp
│       │   └── AQUAComponent.cpp        (new)
│       ├── Rendering/
│       │   ├── AQUARenderer.cpp         (new)
│       │   ├── AQUACamera.cpp           (new)
│       │   ├── AQUAMesh.cpp             (new)
│       │   └── AQUAMaterial.cpp         (new)
│       └── Physics/
│           ├── AQUAPhysicsScene.cpp     (new, PhysX)
│           ├── AQUARigidBody.cpp        (new, PhysX)
│           └── AQUACollisionShape.cpp   (new, PhysX)
└── editor/
    ├── EditorMain.cpp
    └── (future: viewport, hierarchy, inspector, serialization usage)
```

---

## 6. Dependencies and Build

- **AQUA** core depends on **OmegaGTE** (and transitively OmegaCommon). The **Physics** module adds a dependency on **NVIDIA PhysX**; make it optional via CMake (e.g. `AQUA_USE_PHYSX`) so headless or minimal builds can omit it if required.
- Do not add OmegaWTK to the engine core so the runtime can stay editor-free.
- **Editor** executable depends on **AQUA**, **OmegaGTE**, and **OmegaWTK**; it creates the GTE device and passes a render target (from a WTK viewport) to AQUARenderer.
- CMake: add `engine/src/Rendering/*.cpp` to AQUA target; add `engine/src/Physics/*.cpp` when PhysX is enabled; add an `editor` target that links AQUA and OmegaWTK and uses the same GTE backend (Metal/D3D12/Vulkan) as the rest of the project. PhysX can be found via `find_package(PhysX)` or a submodule/vendored build.

---

## 7. Summary

This design turns AQUA from a stub into a working engine by:

1. **Fixing the stub**: Transform constructor, Object/Entity/Scene consistency, and a clear SceneRenderer → AQUARenderer path.
2. **Introducing a minimal but complete pipeline**: Application, Scene, Entity, Component, Renderer, Camera, Mesh, Material, all backed by OmegaGTE and a single draw path.
3. **Using PhysX for physics**: NVIDIA PhysX is the standard for physics; AQUA wraps it with AQUAPhysicsScene, AQUARigidBody, and AQUACollisionShape, and syncs entity transforms each frame so rendering and gameplay see consistent poses.
4. **Keeping the editor separate but integrated**: OmegaWTK hosts the UI; AQUA owns the scene and rendering; the editor drives AQUA and displays the result in a viewport.

Following the phased plan keeps each step testable (e.g. “first frame” in Phase 2, “scene with multiple objects” in Phase 3, physics-driven objects in Phase 5) and aligns with AGENTS.md: Unity-like simplicity for users, Unreal-like 3D capability in the design, PhysX for physics wherever possible, and no duplication of OmegaGTE’s responsibilities.
