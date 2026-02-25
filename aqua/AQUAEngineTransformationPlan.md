# AQUA Engine Transformation Plan

## Goal
Transform `AQUA` from scaffold into a production-capable 3D game engine with:
- modern real-time rendering
- scalable gameplay/runtime systems
- robust asset/content pipeline
- full editor workflows
- profiling, testing, and shipping toolchain

## Mandatory Constraints (from `aqua/AGENTS.md`)
- Design direction is inspired by Unreal Engine architecture.
- Engine code must be modular.
- Engine code files use PascalCase and `AQUA<Name>` naming.

## Current Scaffold Snapshot
- Existing modules are minimal: `AQUABase`, `AQUAObject`, `AQUAScene`, `AQUAFrontend`.
- `AQUAScene.cpp` references `PhysObject`, but `PhysObject` is commented out in headers.
- `AQUAMesh.h` is empty and `AQUACore.h` is only include wiring.
- Renderer is declared (`SceneRenderer`) but not implemented.
- Editor entrypoint exists (`EditorMain.cpp`) but no tooling/runtime integration.

This means Phase 0 must harden contracts before adding systems.

## Target Engine Architecture
Planned engine modules (each as isolated `AQUA<Name>` module/package):
- `AQUACore`: memory, containers, reflection hooks, IDs, logging, config, runtime lifecycle.
- `AQUAPlatform`: window/input/time/filesystem abstraction.
- `AQUAJobs`: task graph, worker pools, frame scheduling.
- `AQUAScene`: world partition, entities/components, transform hierarchy, streaming.
- `AQUARender`: render graph, material/shader system, visibility/culling, lighting.
- `AQUAPhysics`: broadphase/narrowphase, rigid bodies, controllers, queries.
- `AQUAAnimation`: skeletons, blend trees, state machines, IK, retargeting.
- `AQUAAudio`: spatial audio, bus/mixer graph, streaming.
- `AQUAAssets`: importers, cooking, derived data cache, bundle packaging.
- `AQUAScript`: gameplay scripting and hot reload boundaries.
- `AQUANet`: replication, prediction, rollback support tiers.
- `AQUAEditor`: scene editor, inspectors, content browser, play-in-editor.
- `AQUATools`: command-line build/cook/package/profile tools.
- `AQUATest`: unit/integration/perf/golden tests.

## Phased Execution Plan

## Phase 0: Foundation and Contract Repair (Weeks 1-3)
Deliverables:
- compile-clean AQUA core with strict API contracts
- corrected `Scene/Object` type model
- module boundaries and CMake targets for each subsystem shell
- coding standards and naming lints for `AQUA<Name>`

Work:
- fix object model mismatch (`Object` vs `PhysObject`) and define authoritative entity model.
- establish `AQUAResult`, `AQUAHandle`, `AQUAId`, `AQUALog` primitives.
- split headers by module and enforce public/private include layout.
- add CI gates: format, static analysis, unit tests, compile on supported platforms.

Exit Criteria:
- engine builds on target platforms with zero scaffold inconsistencies.
- CI runs on every change.

## Phase 1: Runtime Core (Weeks 4-8)
Deliverables:
- ECS-style runtime with scene graph bridge
- deterministic frame loop and subsystem tick order
- serialization for scene/prefab assets

Work:
- implement `AQUAEntity`, `AQUAComponent`, `AQUAWorld`, `AQUASystem`.
- add transform hierarchy, prefab instancing, and scene streaming primitives.
- implement versioned scene format and binary runtime format.

Exit Criteria:
- can load, simulate, and save a non-trivial scene deterministically.

## Phase 2: Rendering Vertical Slice (Weeks 9-15)
Deliverables:
- real render pipeline on top of OmegaGTE
- PBR materials and shader permutation management
- shadowing, post-process stack, and debug overlays

Work:
- implement `AQUARenderGraph` with explicit pass dependencies.
- add GPU resource lifetime manager, descriptor/pipeline caches.
- support mesh/texture/material import path and runtime binding.
- implement visibility culling (frustum + occlusion tier).

Exit Criteria:
- playable sample level at target FPS budget with stable frame pacing.

## Phase 3: Physics and Gameplay Simulation (Weeks 16-20)
Deliverables:
- rigid body dynamics and collision pipeline
- character controller and scene queries
- gameplay event bridge between runtime, physics, and scripting

Work:
- define `AQUACollider`, `AQUARigidBody`, `AQUAPhysicsScene`.
- integrate fixed-step simulation with interpolation/extrapolation.
- expose raycast/sweep/overlap query APIs.

Exit Criteria:
- stable physical gameplay sample with reproducible simulation behavior.

## Phase 4: Animation, Audio, and VFX (Weeks 21-26)
Deliverables:
- skeletal animation pipeline
- spatial audio runtime
- particle/VFX framework integrated with render graph

Work:
- implement blend trees, animation states, root motion, and event notifies.
- add audio mixer graph and 3D attenuation/occlusion hooks.
- add GPU particle pipeline with authoring metadata.

Exit Criteria:
- character + environment demo with synchronized animation/audio/VFX.

## Phase 5: Asset Pipeline and Cooking (Weeks 27-31)
Deliverables:
- deterministic asset import/cook pipeline
- platform-specific package generation
- incremental build cache and content validation

Work:
- define source asset schemas and cooked artifact schema.
- implement offline tools for texture/mesh/material/animation processing.
- add derived data cache keys and invalidation rules.

Exit Criteria:
- cold + incremental asset builds are reliable and measurable.

## Phase 6: Editor (AQUAEditor) (Weeks 32-38)
Deliverables:
- scene viewport, hierarchy, inspector, content browser
- gizmos, prefab workflows, and play-in-editor loop
- undo/redo transaction system

Work:
- integrate OmegaWTK UI shell with runtime/editor bridge.
- implement editor data model separate from runtime state.
- add hot-reload-safe boundaries for assets and scripts.

Exit Criteria:
- designers can author scenes and iterate without command-line usage.

## Phase 7: Networking and Multiplayer (Weeks 39-45)
Deliverables:
- authority model and replication graph
- client prediction/reconciliation for movement and gameplay primitives
- bandwidth and replication profiling tools

Work:
- define replicated component model and delta serialization.
- implement snapshot/interpolation pipeline and late-join sync.
- expose networking tests and simulation fuzzing.

Exit Criteria:
- stable multiplayer sample under packet loss/latency test profiles.

## Phase 8: Performance, Stability, and Shipping (Weeks 46-52)
Deliverables:
- profiler suite (CPU/GPU/memory/content streaming)
- crash handling, telemetry, regression dashboards
- packaging/deployment flow for target platforms

Work:
- add frame capture tools and long-session soak tests.
- add memory fragmentation tracking and leak detection.
- create release criteria checklist and LTS branch strategy.

Exit Criteria:
- engine meets performance budgets and release quality gates.

## Cross-Cutting Practices (Every Phase)
- Maintain strict module boundaries and no circular dependencies.
- Keep all new engine code in `AQUA<Name>` PascalCase files.
- Require tests for all core runtime/render/physics contracts.
- Track budgets continuously: frame time, memory, load time, package size.
- Maintain a living architecture decision log.

## Immediate Backlog (First 10 Implementation Tasks)
1. Repair `Scene::addObject` contract mismatch and reintroduce concrete physical entity model.
2. Fill `AQUAMesh` API and runtime mesh data layout.
3. Define `AQUAEntity` and component storage archetypes.
4. Introduce `AQUAJobs` worker pool and frame task graph.
5. Implement minimal `AQUARenderGraph` with depth prepass + base pass.
6. Add material system (`AQUAMaterial`, `AQUAShader`, `AQUAPipelineState`).
7. Add scene serialization (`.aquascene`) and prefab format (`.aquaprefab`).
8. Add physics scene step API and query layer.
9. Build editor viewport + hierarchy panel integration.
10. Add CI matrix and benchmark harness.

## Acceptance Metrics for “Fully Capable”
- Visual: PBR, dynamic shadows, post-processing, animation, VFX.
- Runtime: large scene streaming, deterministic simulation, stable hot reload.
- Tooling: full editor authoring loop with undo/redo and profiling.
- Production: automated tests, reproducible builds, platform packaging.
- Multiplayer: replication and prediction for representative gameplay loops.

## Suggested Repository Structure (Target)
```text
aqua/
  engine/
    include/aqua/
      Core/        (AQUACore, AQUAEntity, AQUAComponent, ...)
      Platform/    (AQUAPlatform...)
      Jobs/        (AQUAJobs...)
      Scene/       (AQUAScene, AQUAWorld...)
      Render/      (AQUARender...)
      Physics/     (AQUAPhysics...)
      Animation/   (AQUAAnimation...)
      Audio/       (AQUAAudio...)
      Assets/      (AQUAAssets...)
      Script/      (AQUAScript...)
      Net/         (AQUANet...)
    src/
      Core/ Platform/ Jobs/ Scene/ Render/ Physics/ Animation/ Audio/ Assets/ Script/ Net/
  editor/
    src/           (AQUAEditor...)
  tools/
    src/           (AQUATools...)
  tests/
    unit/ integration/ perf/
```

## Governance
- Use milestone-driven releases: `0.1 Core`, `0.2 Render`, `0.3 Gameplay`, `0.4 Editor`, `0.5 Network`, `1.0 Production`.
- Freeze APIs at each milestone and allow only additive changes during stabilization windows.
- Run monthly architecture reviews focused on performance deltas and dependency hygiene.
