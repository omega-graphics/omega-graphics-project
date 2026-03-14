# AQUA Game Engine: Phase-by-Phase Implementation Plan

This document turns the [AQUA Design Document](AQUA-Design-Stub-to-Engine.md) into a concrete, phase-by-phase implementation plan. Each phase lists tasks in order, files to create or modify, and how to verify the phase is complete.

---

## Phase 1: Core and Scene Consistency

**Goal:** Fix stub inconsistencies, introduce a single “scene node” type, add application lifecycle, and wire the editor to AQUA—no rendering yet.

**Prerequisites:** None (start from current stub).

### 1.1 Transform

| # | Task | Details |
|---|------|---------|
| 1 | Implement `Transform` constructor | In `engine/src/Core/AQUAObject.cpp` (or new `AQUATransform.cpp`), define `Transform::Transform(float x, float y, float z, float pitch, float yaw, float roll)` that initializes all six members. |
| 2 | Allow default (identity) transform | In `engine/include/aqua/Core/AQUAObject.h`, either remove `Transform() = delete` and add a default constructor that sets e.g. position 0,0,0 and rotation 0,0,0, or document that Transform is always explicitly constructed. Prefer allowing default for use in containers. |

**Files:** `engine/include/aqua/Core/AQUAObject.h`, `engine/src/Core/AQUAObject.cpp` (or new `AQUATransform.h` / `AQUATransform.cpp`).

**Done when:** Transform can be default-constructed and 6-param constructed; `Translate` and `Rotate` still work.

---

### 1.2 Resolve Object / Entity / PhysObject

| # | Task | Details |
|---|------|---------|
| 1 | Choose canonical scene node type | Decide: either (A) uncomment and fix `PhysObject` in `AQUAObject.h` as the scene entity with Transform, or (B) introduce a new `Entity` class (e.g. in `AQUAEntity.h`) that extends `Object` and has a Transform. Design doc prefers one clear type (Entity). |
| 2 | Align Scene with chosen type | In `AQUAScene.h`, set `objectContainer` to `Vector<SharedHandle<Entity>>` (or `PhysObject`). In `AQUAScene.cpp`, change `addObject` to take the same type and push into `objectContainer`. Remove any reference to the other type. |
| 3 | Give scene node a Transform | Ensure the chosen type (Entity or PhysObject) has a `Transform` member and optional parent/child or name/ID for later hierarchy. |

**Files:** `engine/include/aqua/Core/AQUAObject.h`, `engine/include/aqua/Scene/AQUAScene.h`, `engine/src/Scene/AQUAScene.cpp`; optionally `engine/include/aqua/Core/AQUAEntity.h`, `engine/src/Core/AQUAEntity.cpp`.

**Done when:** One type is used consistently for “thing in the scene”; Scene compiles and can add/remove that type.

---

### 1.3 AQUAApplication

| # | Task | Details |
|---|------|---------|
| 1 | Add AQUAApplication header | Create `engine/include/aqua/Core/AQUAApplication.h` with a class that has: `Init()` (or static `Create()`), `Run()` or `Tick()` (single step), `Shutdown()`. It should hold or create nothing for GTE yet—placeholder only. |
| 2 | Implement AQUAApplication | Create `engine/src/Core/AQUAApplication.cpp` with stub implementations: Init returns success, Tick does nothing, Shutdown clears state. |
| 3 | Optional: singleton or explicit instance | Document or implement whether the app is a singleton or passed explicitly; ensure Frontend can call Init/Tick/Shutdown. |

**Files:** `engine/include/aqua/Core/AQUAApplication.h`, `engine/src/Core/AQUAApplication.cpp`.

**Done when:** Code can create an AQUAApplication, call Init(), then Tick() (e.g. in a loop), then Shutdown().

---

### 1.4 AQUAEntity and AQUAComponent (minimal)

| # | Task | Details |
|---|------|---------|
| 1 | Add base Component type | Create `engine/include/aqua/Scene/AQUAComponent.h` with an abstract or empty base class (e.g. `class Component` or `AQUAComponent`) that entities can hold. No concrete components yet. |
| 2 | Entity holds Transform and components | If Entity was introduced in 1.2, ensure it has: Transform, optional parent/child or list of root entities, and a list (or slot) for Components. Implement in `AQUAEntity.cpp` if created. |
| 3 | Scene stores entities only | Scene’s container holds only the chosen entity type; no render-specific types in Scene. |

**Files:** `engine/include/aqua/Scene/AQUAComponent.h`, optionally `engine/include/aqua/Core/AQUAEntity.h`, `engine/src/Core/AQUAEntity.cpp`.

**Done when:** Scene contains entities; each entity has a Transform and can hold (possibly zero) components.

---

### 1.5 Editor links AQUA and runs a minimal loop

| # | Task | Details |
|---|------|---------|
| 1 | Add editor target in CMake | In `aqua/CMakeLists.txt` (or parent), add an executable or OmegaWTK app target for the editor that links AQUA and OmegaGTE. Do not add OmegaWTK to the AQUA library. |
| 2 | Editor creates Application and Scene | In `editor/EditorMain.cpp`, after OmegaWTK startup: create AQUAApplication, call Init(), create one empty Scene (and optionally one Entity), then either run a short loop calling Tick() or a single Tick(). Call Shutdown() before exit. |
| 3 | Build and run | Ensure the editor binary builds and runs without crashing; no viewport or rendering yet. |

**Files:** `aqua/CMakeLists.txt`, `aqua/editor/EditorMain.cpp`.

**Done when:** Editor launches, initializes AQUA, creates an empty scene, runs one or more ticks, shuts down cleanly.

---

### Phase 1 summary

| Deliverable | Description |
|-------------|-------------|
| Transform | Constructors implemented; default allowed. |
| Scene node type | Single consistent type (Entity or PhysObject) in Scene. |
| AQUAApplication | Init / Tick / Shutdown; no GTE yet. |
| Component base | Base type for future mesh/camera/light components. |
| Editor | Links AQUA; creates Application + empty Scene; runs tick loop. |

---

## Phase 2: First Frame with OmegaGTE

**Goal:** Drive OmegaGTE from AQUA: create GTE and a render target from a window, then draw one frame (e.g. clear + fullscreen quad or cube) and present.

**Prerequisites:** Phase 1 done; OmegaGTE built and linkable.

### 2.1 AQUARenderer and GTE setup

| # | Task | Details |
|---|------|---------|
| 1 | Create AQUARenderer header | Add `engine/include/aqua/Rendering/AQUARenderer.h` with a class that holds e.g. `SharedHandle<OmegaGraphicsEngine>`, `SharedHandle<GENativeRenderTarget>`, and optionally `GECommandQueue`. Add `beginFrame()`, `endFrame()` (or `present()`), and a method to set or create the render target from a native window handle. |
| 2 | Implement AQUARenderer | Add `engine/src/Rendering/AQUARenderer.cpp`. In implementation, use OmegaGTE APIs: create GTE (e.g. `InitWithDefaultDevice()`), create `GENativeRenderTarget` from the window handle. In beginFrame, get a command buffer; in endFrame, submit and present. |
| 3 | Add Rendering to CMake | In `aqua/CMakeLists.txt`, add `engine/src/Rendering/*.cpp` (or list AQUARenderer.cpp) to the AQUA target; ensure include path and OmegaGTE link are correct. |

**Files:** `engine/include/aqua/Rendering/AQUARenderer.h`, `engine/src/Rendering/AQUARenderer.cpp`, `aqua/CMakeLists.txt`.

**Done when:** AQUARenderer can be constructed with a window handle, and beginFrame/endFrame run without crashing (clear only is fine).

---

### 2.2 AQUACamera (data only)

| # | Task | Details |
|---|------|---------|
| 1 | Add AQUACamera header | Create `engine/include/aqua/Rendering/AQUACamera.h` with a struct or class holding: FOV, near plane, far plane, viewport (or aspect). Optional: helper to compute view/projection matrix from Transform. |
| 2 | Implement if needed | If only data, implementation can be empty or in header; if matrix helper is added, add `engine/src/Rendering/AQUACamera.cpp`. |

**Files:** `engine/include/aqua/Rendering/AQUACamera.h`, optionally `engine/src/Rendering/AQUACamera.cpp`.

**Done when:** Camera parameters can be set and read; optional view/projection available for renderer.

---

### 2.3 AQUAMesh and AQUAMaterial (minimal)

| # | Task | Details |
|---|------|---------|
| 1 | AQUAMesh | Create `engine/include/aqua/Rendering/AQUAMesh.h` (and .cpp if needed). Hold a GTE buffer (vertex + index) or reference to one, and topology. For Phase 2, a single hardcoded quad or cube is enough (e.g. 4 or 8 vertices, 6 or 12 indices). |
| 2 | AQUAMaterial | Create `engine/include/aqua/Rendering/AQUAMaterial.h` (and .cpp). Hold or reference a `GERenderPipelineState` and any bindings (can be empty for a single unlit pass). |
| 3 | Create default pipeline and mesh in renderer | In AQUARenderer (or a small helper), create one pipeline state (e.g. from a built-in or minimal OmegaSL shader) and one mesh (quad or cube). |

**Files:** `engine/include/aqua/Rendering/AQUAMesh.h`, `engine/src/Rendering/AQUAMesh.cpp`, `engine/include/aqua/Rendering/AQUAMaterial.h`, `engine/src/Rendering/AQUAMaterial.cpp`.

**Done when:** Renderer can create one mesh and one material; draw one draw call (e.g. fullscreen quad or cube) inside beginFrame/endFrame.

---

### 2.4 End-to-end first frame

| # | Task | Details |
|---|------|---------|
| 1 | Draw path in AQUARenderer | In beginFrame: clear render target, set viewport; bind the default pipeline and mesh; issue one draw; in endFrame: submit and present. |
| 2 | Wire to Application or Frontend | AQUAApplication (or Frontend) creates a window (or gets it from WTK), creates AQUARenderer with that window, and each Tick() calls beginFrame(), draw, endFrame(). |
| 3 | Test | Run the app (standalone or editor); confirm one frame appears (e.g. single color or simple quad/cube). |

**Files:** `engine/src/Rendering/AQUARenderer.cpp`, `engine/src/Core/AQUAApplication.cpp` or `engine/src/AQUAFrontend.cpp` (if implemented).

**Done when:** One frame is rendered to the window using OmegaGTE via AQUA.

---

### Phase 2 summary

| Deliverable | Description |
|-------------|-------------|
| AQUARenderer | Owns GTE and render target; beginFrame / endFrame / present. |
| AQUACamera | Data only; FOV, near/far, viewport. |
| AQUAMesh / AQUAMaterial | One default mesh and one pipeline; used by renderer. |
| First frame | Clear + one draw + present from AQUA. |

---

## Phase 3: Scene-Driven Rendering

**Goal:** Renderer reads from Scene: traverse entities, collect camera and mesh components, and draw all meshes (one camera, multiple meshes); optional simple lighting.

**Prerequisites:** Phase 1 and 2 done.

### 3.1 Camera and mesh components

| # | Task | Details |
|---|------|---------|
| 1 | AQUACameraComponent | Add a component type that holds AQUACamera data (or a reference). In `engine/include/aqua/Scene/AQUACameraComponent.h` (or under Rendering), define it; ensure it can be attached to an Entity. |
| 2 | AQUAMeshComponent | Add a component that holds a reference to AQUAMesh (or mesh ID) and AQUAMaterial (or material ID). Attachable to Entity. |
| 3 | Register in Scene / Entity | Ensure Scene (or Entity) can enumerate entities and their components; renderer will query for camera and mesh components. |

**Files:** `engine/include/aqua/Scene/AQUACameraComponent.h`, `engine/include/aqua/Scene/AQUAMeshComponent.h` (or under Rendering), and corresponding .cpp if needed.

**Done when:** Entities can have at most one camera component and one or more mesh components; scene can be traversed to find them.

---

### 3.2 Renderer uses Scene

| # | Task | Details |
|---|------|---------|
| 1 | render(Scene) or render(const Scene&) | Add a method on AQUARenderer that takes the current Scene (or list of entities). Implement: find first (or main) camera component for view/projection; iterate entities with mesh components; for each, get transform (from entity), bind mesh and material, submit draw. |
| 2 | Transform to matrix | Convert entity Transform to a world matrix (or use Transform directly if GTE accepts it) and pass to the pipeline (e.g. per-object constant buffer). |
| 3 | Culling | Optional: trivial culling (e.g. frustum with camera); can be a no-op at first. |

**Files:** `engine/include/aqua/Rendering/AQUARenderer.h`, `engine/src/Rendering/AQUARenderer.cpp`.

**Done when:** Passing a Scene with one camera and several meshes results in all meshes drawn with correct transforms.

---

### 3.3 Optional: simple lighting

| # | Task | Details |
|---|------|---------|
| 1 | Light component (data) | Add a simple light component (directional or point): direction/position, color, intensity. No PhysX or rendering logic beyond data. |
| 2 | Pass to renderer | Renderer collects light components and passes them to the pipeline (e.g. one directional light in a constant buffer). Extend the default shader to use it. |

**Files:** `engine/include/aqua/Scene/AQUALightComponent.h` (or Rendering), shader or pipeline update.

**Done when:** One light affects the scene (e.g. directional); optional for phase completion.

---

### Phase 3 summary

| Deliverable | Description |
|-------------|-------------|
| Camera / mesh components | Attachable to entities; scene can be queried. |
| Scene-driven draw | One camera, N meshes; transforms applied. |
| Optional lighting | One light (e.g. directional) in a simple pass. |

---

## Phase 4: Editor Integration

**Goal:** Editor viewport shows the current scene; hierarchy and inspector for entities and transforms; scene load/save.

**Prerequisites:** Phase 1–3 done; OmegaWTK builds and runs.

### 4.1 Viewport in OmegaWTK

| # | Task | Details |
|---|------|---------|
| 1 | Viewport widget or panel | In the editor, add a widget/panel that can provide a native window handle (or GTE-compatible render target). On resize, notify or recreate render target if needed. |
| 2 | Render current scene to viewport | Each editor frame (or on timer): get current Scene from AQUAApplication (or editor state), call AQUARenderer with the viewport’s render target and that scene. Use the same GTE device/context as the rest of the app. |
| 3 | Camera for viewport | Either use the scene’s main camera or a dedicated editor camera (e.g. orbit) for the viewport; ensure the renderer can use it for view/projection. |

**Files:** `editor/` (viewport widget, integration with AQUARenderer); possibly `engine/include/aqua/Rendering/AQUARenderer.h` to accept an external render target.

**Done when:** Editor viewport displays the 3D scene rendered by AQUA.

---

### 4.2 Hierarchy and inspector

| # | Task | Details |
|---|------|---------|
| 1 | Hierarchy panel | List all entities in the current scene (by name or ID); support selection. Optional: tree view if parent/child is implemented. |
| 2 | Inspector panel | When an entity is selected, show its Transform (position, rotation, scale) and list of components. Allow editing transform (and later component properties). |
| 3 | Push changes to AQUA | Edits in the inspector update the selected entity’s Transform (and components) in the AQUA scene so the next render reflects changes. |

**Files:** `editor/` (UI for hierarchy and inspector; calls into AQUA scene/entity API).

**Done when:** User can select an entity in the hierarchy and edit its transform in the inspector; viewport updates.

---

### 4.3 Scene load/save

| # | Task | Details |
|---|------|---------|
| 1 | Serialization format | Choose a format (e.g. JSON or simple binary) for Scene + entities + transforms + component references. Define schema or document. |
| 2 | AQUASceneSerialization (optional in engine) | Add `engine/include/aqua/Scene/AQUASceneSerialization.h` and .cpp: `saveScene(Scene, path)`, `loadScene(path) -> SharedHandle<Scene>`. Engine stays agnostic of editor; both game and editor can use these. |
| 3 | Editor: load/save menu | Editor menus or buttons: Load Scene, Save Scene. Call serialization and set the loaded scene as current (or merge). |

**Files:** `engine/include/aqua/Scene/AQUASceneSerialization.h`, `engine/src/Scene/AQUASceneSerialization.cpp`, `editor/` (menu and file dialogs).

**Done when:** User can save the current scene to a file and load it back; hierarchy and viewport reflect the loaded scene.

---

### Phase 4 summary

| Deliverable | Description |
|-------------|-------------|
| Viewport | Editor viewport renders current scene via AQUARenderer. |
| Hierarchy | List entities; selection. |
| Inspector | Edit selected entity transform (and basic component data). |
| Load/save | Serialize/deserialize scene; editor can load and save. |

---

## Phase 5: Physics (PhysX)

**Goal:** Integrate NVIDIA PhysX: scene, rigid bodies, collision shapes; sync entity transforms each frame; physics step in main loop.

**Prerequisites:** Phase 1–4 (or at least 1–3); PhysX SDK available (build or package).

### 5.1 PhysX dependency and build

| # | Task | Details |
|---|------|---------|
| 1 | Obtain PhysX | Add PhysX as submodule, vendored source, or `find_package(PhysX)`; ensure the project builds PhysX (or links to prebuilt) for the target platforms. |
| 2 | CMake option | Add an option (e.g. `AQUA_USE_PHYSX`) to enable/disable the Physics module. When disabled, do not compile or link PhysX. |
| 3 | AQUA Physics sources | Add `engine/src/Physics/*.cpp` to the AQUA target only when PhysX is enabled; add PhysX include dirs and libraries. |

**Files:** `aqua/CMakeLists.txt`, possibly `cmake/FindPhysX.cmake` or similar.

**Done when:** With AQUA_USE_PHYSX=ON, AQUA builds and links PhysX; with OFF, build succeeds without Physics.

---

### 5.2 AQUAPhysicsScene

| # | Task | Details |
|---|------|---------|
| 1 | AQUAPhysicsScene header | Create `engine/include/aqua/Physics/AQUAPhysicsScene.h`. Class that wraps a PhysX `PxScene` (or equivalent); methods: create (or init), destroy, `step(dt)`. Optional: set gravity, solver config. |
| 2 | Implementation | Create `engine/src/Physics/AQUAPhysicsScene.cpp`. Initialize PhysX foundation and physics if needed; create PxScene; in step(), call scene simulate and fetch results. |
| 3 | Ownership | One AQUAPhysicsScene per AQUA Scene (or one global); document and implement lifecycle (create when scene loads, destroy when unload). |

**Files:** `engine/include/aqua/Physics/AQUAPhysicsScene.h`, `engine/src/Physics/AQUAPhysicsScene.cpp`.

**Done when:** AQUAPhysicsScene can be created, stepped with a dt, and destroyed without crash.

---

### 5.3 AQUARigidBody and transform sync

| # | Task | Details |
|---|------|---------|
| 1 | AQUARigidBody header | Create `engine/include/aqua/Physics/AQUARigidBody.h`. Component or resource that holds a PhysX rigid body (PxRigidDynamic or PxRigidStatic). Methods or properties: set mass, kinematic flag; sync from Transform to PhysX (for kinematic); sync from PhysX to Transform (after step). |
| 2 | Implementation | Create `engine/src/Physics/AQUARigidBody.cpp`. Create/destroy PxRigidDynamic/PxRigidStatic; attach to PxScene. Implement sync: entity Transform → set global pose on actor (kinematic); after scene step, read global pose from actor → entity Transform. |
| 3 | Attach to entity | Rigid body component is attached to an entity; it uses that entity’s Transform for sync. Ensure the entity is in the same scene as the AQUAPhysicsScene. |

**Files:** `engine/include/aqua/Physics/AQUARigidBody.h`, `engine/src/Physics/AQUARigidBody.cpp`.

**Done when:** Adding a dynamic rigid body to an entity and stepping the physics scene updates the entity’s transform; rendering shows the moved object.

---

### 5.4 AQUACollisionShape

| # | Task | Details |
|---|------|---------|
| 1 | AQUACollisionShape header | Create `engine/include/aqua/Physics/AQUACollisionShape.h`. Wrapper for PhysX shapes: box, sphere, capsule (and optionally convex/triangle mesh). Create shape, attach to rigid body. |
| 2 | Implementation | Create `engine/src/Physics/AQUACollisionShape.cpp`. Create PxBoxGeometry, PxSphereGeometry, PxCapsuleGeometry, etc.; create PxShape and attach to the rigid body actor. |

**Files:** `engine/include/aqua/Physics/AQUACollisionShape.h`, `engine/src/Physics/AQUACollisionShape.cpp`.

**Done when:** Rigid bodies can have box/sphere/capsule collision shapes; simulation runs and collisions are resolved.

---

### 5.5 Main loop integration

| # | Task | Details |
|---|------|---------|
| 1 | Physics step before render | In AQUAApplication::Tick() (or main loop): (1) Copy kinematic entity transforms to PhysX actors; (2) call AQUAPhysicsScene::step(dt); (3) copy dynamic actor poses back to entity transforms; (4) run scene update (scripts if any); (5) render. |
| 2 | Fixed or variable dt | Choose fixed (e.g. 1/60) or variable timestep for physics; document and implement. Optionally cap dt to avoid large steps. |

**Files:** `engine/src/Core/AQUAApplication.cpp` (or wherever the main loop lives).

**Done when:** Main loop runs physics step then render; dynamic bodies move and are drawn in the correct place.

---

### Phase 5 summary

| Deliverable | Description |
|-------------|-------------|
| PhysX in build | Optional Physics module; links PhysX when enabled. |
| AQUAPhysicsScene | Create, step(dt), destroy. |
| AQUARigidBody | Sync Transform ↔ PhysX actor; dynamic and kinematic. |
| AQUACollisionShape | Box, sphere, capsule (and optionally more). |
| Main loop | Physics step → sync → render. |

---

## Phase 6: Polish and Extension

**Goal:** Input, extended PhysX features, audio placeholder, documentation and samples.

**Prerequisites:** Phases 1–5 (or at least 1–3 and 5 for a physics-focused path).

### 6.1 Input module

| # | Task | Details |
|---|------|---------|
| 1 | Input API | Add a minimal input module: keyboard key state, mouse position/buttons (and optionally gamepad). Can be platform-specific or abstracted; editor may use OmegaWTK input for viewport. |
| 2 | Hook into main loop | Expose input state to game/script layer; optional: input events or polling in Tick(). |

**Files:** e.g. `engine/include/aqua/Input/AQUAInput.h`, `engine/src/Input/AQUAInput.cpp`; integrate in Application or Frontend.

**Done when:** Game or sample can read keyboard/mouse state each frame.

---

### 6.2 Extended PhysX (optional)

| # | Task | Details |
|---|------|---------|
| 1 | Character controller | If needed, wrap PhysX character controller; expose move/collision with world. |
| 2 | Triggers and raycasts | Expose trigger volumes and raycast (or sweep) queries for gameplay. |
| 3 | Cloth/particles | Only if required; defer to later. |

**Files:** `engine/include/aqua/Physics/` additions; implementation in Physics module.

**Done when:** (Optional) Character controller and/or raycasts available where needed.

---

### 6.3 Audio placeholder

| # | Task | Details |
|---|------|---------|
| 1 | Stub or minimal API | Add an audio module stub: e.g. play sound by ID, set listener (from camera). Implementation can be no-op or minimal (e.g. one backend). |
| 2 | Listener from camera | Optional: set audio listener transform from main camera each frame. |

**Files:** e.g. `engine/include/aqua/Audio/AQUAAudio.h`, `engine/src/Audio/AQUAAudio.cpp`.

**Done when:** Audio API exists; can be extended later without breaking design.

---

### 6.4 Documentation and samples

| # | Task | Details |
|---|------|---------|
| 1 | README and build instructions | Update `aqua/README.md` with how to build AQUA (with/without PhysX, editor), and how to run a minimal game or editor. |
| 2 | Sample: blank scene | Minimal sample that creates a scene, one camera, no meshes; runs and renders (clear only or one quad). |
| 3 | Sample: cube + camera | Scene with one camera and one cube (mesh); demonstrates scene-driven rendering. |
| 4 | Sample: rigid body + collision | Scene with static ground and one or two dynamic boxes; demonstrates physics and transform sync. |

**Files:** `aqua/README.md`, sample projects or a single demo app under `aqua/samples/` or similar.

**Done when:** New contributors can build and run at least one sample; docs describe the phases and main modules.

---

### Phase 6 summary

| Deliverable | Description |
|-------------|-------------|
| Input | Keyboard/mouse (and optionally gamepad); polled or event-based. |
| PhysX extensions | Optional: character controller, triggers, raycasts. |
| Audio | Stub or minimal API. |
| Docs and samples | README, blank scene, cube+camera, physics sample. |

---

## Dependency overview

```
Phase 1 ────────────────────────────────────────────────────────────────┐
    │                                                                     │
    ├── Phase 2 (first frame)                                            │
    │       │                                                            │
    │       └── Phase 3 (scene-driven rendering)                         │
    │               │                                                    │
    │               └── Phase 4 (editor viewport, hierarchy, load/save)  │
    │                                                                     │
    └── Phase 5 (PhysX) ───────────────────────────────────────────────┘
                    │
                    └── Phase 6 (input, audio, docs, samples)
```

- Phase 4 can start once Phase 3 is in place (viewport needs scene-driven renderer).
- Phase 5 can be done in parallel with 3/4 once Core and Scene (Phase 1) and optionally rendering (Phase 2/3) exist; main loop integration (5.5) assumes a tick loop exists (Phase 1).
- Phase 6 items are largely independent; can be picked by priority.

---

## Checklist: am I done with a phase?

- **Phase 1:** Transform works; one scene node type; Application init/tick/shutdown; editor runs and ticks with empty scene.
- **Phase 2:** One frame drawn via AQUA (clear + one mesh + present).
- **Phase 3:** Scene with multiple entities and mesh/camera components renders correctly.
- **Phase 4:** Editor viewport shows scene; hierarchy and inspector edit transforms; load/save scene.
- **Phase 5:** PhysX scene steps; rigid bodies and shapes work; transforms sync; main loop runs physics then render.
- **Phase 6:** Input and audio stubs exist; at least one sample and README updated.
