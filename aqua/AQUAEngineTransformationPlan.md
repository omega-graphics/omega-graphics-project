# AQUA Engine Transformation Plan (Constraint-Reevaluated)

## Goal
Transform `AQUA` from scaffold into a production-capable 3D game engine that combines:
- Unity-like simplicity for day-to-day gameplay authoring
- Unreal-grade depth for rendering, simulation, tooling, and scale

## Updated Constraints (from `aqua/AGENTS.md`)
- Architecture is inspired by both Unreal Engine and Unity.
- Do not recycle old concepts or naming; use modern, water-themed naming aligned to AQUA identity.
- Code styling must follow LLVM conventions.
- All code must be modular.
- File names must be PascalCase with the `AQUA<Name>` prefix.

## Constraint Translation Into Hard Engineering Rules

## Rule 1: Dual-Lane Developer Experience
- Every subsystem must expose:
- a simple authoring lane (minimal APIs, clear defaults, low ceremony)
- an advanced lane (explicit control for performance and scale)
- "Simple by default, deep when needed" is a release gate.

## Rule 2: Water-Themed Naming System
- New runtime vocabulary:
- `Tide` for world/scene containers
- `Drop` for entities/objects
- `Trait` for components/data aspects
- `Current` for systems/execution flows
- `Reef` for rendering layers/pipelines
- `Flow` for physics/motion simulation
- `Ripple` for networking/replication
- `Reservoir` for assets/content storage
- `Harbor` for platform/toolchain integration
- New files and classes must follow `AQUA<WaterTerm><Role>` naming.
- Legacy names (`Scene`, `Object`, etc.) may exist only as temporary compatibility wrappers with deprecation tags and migration deadlines.

## Rule 3: LLVM Style As a CI Gate
- Add `.clang-format` based on LLVM style.
- Add `clang-tidy` checks for correctness and modernization.
- Style/lint failures block merges.

## Rule 4: Strong Modularity
- Each subsystem has a standalone module target with explicit public/private boundaries.
- No circular dependencies.
- Runtime/editor/tooling boundaries are enforced in build definitions.

## Current Scaffold Re-Evaluation
- Existing code is skeletal: `AQUABase`, `AQUAObject`, `AQUAScene`, `AQUAFrontend`.
- `AQUAScene.cpp` references `PhysObject`, but `PhysObject` is commented out in headers (broken contract).
- `AQUAMesh.h` is empty and render/frontend layers are stubs.
- `AQUAFrontend.h` defines `class Frontend`, which violates the naming rule (class should be prefixed).
- No LLVM style config/gates are currently visible.
- Current naming does not yet reflect the new water-themed taxonomy.

Phase 0 must stabilize contracts, naming, and style governance before feature expansion.

## Target Module Architecture (Water-Themed + Modular)
- `AQUASourceCore`: IDs, handles, memory utilities, logging, reflection hooks, lifecycle.
- `AQUAHarborPlatform`: windowing, input, timing, filesystem, platform abstraction.
- `AQUACurrentJobs`: worker pools, fiber/task graph, frame orchestration.
- `AQUATideWorld`: world partition, `Drop`/`Trait` storage, hierarchy, streaming.
- `AQUAReefRender`: render graph, materials, shader management, culling, lighting.
- `AQUAFlowPhysics`: collision, rigid bodies, queries, fixed-step simulation.
- `AQUASurgeAnimation`: skeletons, blend graphs, IK, retargeting.
- `AQUAEchoAudio`: spatial audio, mixer graph, stream scheduling.
- `AQUAReservoirAssets`: import/cook/cache/package pipeline.
- `AQUATideScript`: gameplay scripting API with hot-reload boundaries.
- `AQUARippleNet`: replication, prediction, reconciliation, transport adapters.
- `AQUAStudioEditor`: editor shell, inspectors, viewport, authoring workflows.
- `AQUAHarborTools`: command-line build/cook/package/profile tools.
- `AQUATestBench`: unit/integration/perf/golden tests.

## Phased Execution Plan

## Phase 0: Contract and Governance Reset (Weeks 1-4)
Deliverables:
- compile-clean foundation with resolved type/API mismatches
- naming taxonomy and migration policy document
- LLVM style/lint CI gates enabled
- module shells for all target subsystems

Work:
- repair `Scene/Object/PhysObject` contract by introducing `Tide/Drop/Trait` canonical model.
- rename noncompliant symbols (example: `Frontend` -> `AQUAHarborFrontend` or equivalent).
- add deprecation wrappers for legacy names with explicit removal milestones.
- set up `.clang-format` and `clang-tidy` in CI.
- define per-module ownership and dependency matrix.

Exit Criteria:
- no contract mismatches in core scaffold.
- style and lint checks enforce LLVM rules on every PR.
- naming compliance script reports zero violations for new code.

## Phase 1: Runtime Core and Authoring Simplicity (Weeks 5-9)
Deliverables:
- `Drop/Trait/Current/Tide` runtime model
- deterministic frame loop with explicit system ordering
- simple gameplay API facade over advanced internals

Work:
- implement `AQUADrop`, `AQUATrait`, `AQUACurrent`, `AQUATide`.
- provide simple authoring calls:
- create/drop lookup
- trait attach/remove
- event subscription
- provide advanced APIs for memory layout, scheduling, and explicit update phases.
- versioned serialization for tides/prefabs.

Exit Criteria:
- teams can build a small gameplay prototype with simple APIs.
- advanced path supports deterministic replay.

## Phase 2: Rendering Vertical Slice (Weeks 10-16)
Deliverables:
- `AQUAReefRender` production path on OmegaGTE
- PBR materials, shader permutations, and visibility pipeline
- debug visualization and frame diagnostics

Work:
- implement `AQUAReefGraph` pass scheduling and resource lifetimes.
- add pipeline/material cache and descriptor management.
- support mesh/texture/material ingestion from `AQUAReservoirAssets`.
- build frustum + occlusion culling.

Exit Criteria:
- sample 3D level reaches target frame budget with stable pacing.

## Phase 3: Physics and Gameplay Simulation (Weeks 17-21)
Deliverables:
- `AQUAFlowPhysics` scene integration
- rigid bodies, controllers, and query API
- event bridge between flow simulation and gameplay currents

Work:
- implement colliders, rigid bodies, and fixed-step integration.
- add interpolation/extrapolation for render smoothness.
- expose raycast/sweep/overlap at simple and advanced API layers.

Exit Criteria:
- stable, reproducible physics gameplay sample.

## Phase 4: Animation, Audio, and VFX (Weeks 22-27)
Deliverables:
- `AQUASurgeAnimation` with blend state graphs
- `AQUAEchoAudio` spatial pipeline
- GPU particle and effect framework wired into `AQUAReefRender`

Work:
- implement animation state graph, notifies, root motion, and IK hooks.
- add audio bus graph, attenuation, and occlusion integration.
- add particle simulation/render passes and authoring metadata.

Exit Criteria:
- cohesive character/environment demo with synchronized animation/audio/VFX.

## Phase 5: Asset Reservoir and Content Cooking (Weeks 28-32)
Deliverables:
- deterministic import/cook pipeline
- incremental derived-data cache
- platform packaging bundles

Work:
- define source schemas and cooked schemas.
- implement importers and validators for core asset classes.
- add cache keying, invalidation, and content dependency graph.

Exit Criteria:
- reliable cold and incremental content build performance.

## Phase 6: Studio Editor (Weeks 33-39)
Deliverables:
- viewport, hierarchy, inspector, content browser
- play-in-editor and prefab editing
- transaction-based undo/redo

Work:
- integrate OmegaWTK-based UI shell with runtime/editor boundary layer.
- separate authoring state from runtime state.
- enable safe hot reload for scripts/assets.

Exit Criteria:
- scene authoring loop works without command-line dependency.

## Phase 7: Ripple Networking (Weeks 40-46)
Deliverables:
- authority and replication architecture
- movement and gameplay prediction/reconciliation
- network diagnostics and profiling

Work:
- implement replicated trait model and delta serialization.
- add snapshot/interpolation and reconnect/late-join handling.
- build packet loss and latency simulation tests.

Exit Criteria:
- multiplayer sample remains stable under degraded network profiles.

## Phase 8: Production Hardening and Release (Weeks 47-52)
Deliverables:
- CPU/GPU/memory/streaming profiler suite
- crash capture and regression dashboards
- shipping toolchain for target platforms

Work:
- add soak tests and long-session stability runs.
- add memory diagnostics and leak/fragmentation tracking.
- finalize release checklist and LTS branching policy.

Exit Criteria:
- engine passes quality, performance, and release gates for `1.0`.

## Cross-Cutting Compliance Gates (All Phases)
- Naming gate:
- reject new symbols/files that do not use `AQUA<Name>` PascalCase
- reject non-water-themed runtime terms in new core APIs
- Style gate:
- enforce LLVM formatting and linting in CI
- Modularity gate:
- fail builds on circular dependencies or forbidden cross-module includes
- API ergonomics gate:
- every subsystem must demonstrate both simple and advanced usage paths
- Test gate:
- unit + integration + perf coverage required for core runtime systems

## Immediate Backlog (First 12 Tasks)
1. Replace `Scene/Object` contract with `AQUATide` + `AQUADrop` base contracts.
2. Restore physical model using `AQUAFlowBody`/`AQUAFlowCollider` abstractions.
3. Rename `Frontend` class to compliant prefixed/water-themed name.
4. Create naming glossary and deprecated alias map.
5. Add `.clang-format` (LLVM) and CI enforcement.
6. Add `clang-tidy` baseline and module include-boundary checks.
7. Implement `AQUAReefMesh` data model and upload path.
8. Implement `AQUACurrentFrameGraph` for system scheduling.
9. Implement minimal `AQUAReefGraph` (depth + base pass).
10. Define tide/prefab serialization formats with versioning.
11. Add basic editor viewport + hierarchy on the new tide/drop model.
12. Add compile/test matrix and performance benchmark harness.

## Acceptance Metrics for "Fully Capable"
- Simplicity:
- a new gameplay prototype can be assembled using simple lane APIs in under one day
- Depth:
- advanced lane supports streaming worlds, high-fidelity rendering, and deterministic simulation
- Naming/identity:
- all new core APIs and files follow AQUA prefix + water-themed taxonomy
- Code quality:
- LLVM style and lint gates are consistently green
- Tooling:
- editor + asset pipeline + profiler are production-usable
- Networking:
- replication and prediction stable under adverse network conditions

## Target Repository Structure
```text
aqua/
  engine/
    include/aqua/
      SourceCore/      (AQUASourceCore...)
      HarborPlatform/  (AQUAHarborPlatform...)
      CurrentJobs/     (AQUACurrentJobs...)
      TideWorld/       (AQUATideWorld, AQUADrop, AQUATrait...)
      ReefRender/      (AQUAReefRender...)
      FlowPhysics/     (AQUAFlowPhysics...)
      SurgeAnimation/  (AQUASurgeAnimation...)
      EchoAudio/       (AQUAEchoAudio...)
      ReservoirAssets/ (AQUAReservoirAssets...)
      TideScript/      (AQUATideScript...)
      RippleNet/       (AQUARippleNet...)
    src/
      SourceCore/ HarborPlatform/ CurrentJobs/ TideWorld/ ReefRender/
      FlowPhysics/ SurgeAnimation/ EchoAudio/ ReservoirAssets/ TideScript/ RippleNet/
  editor/
    src/               (AQUAStudioEditor...)
  tools/
    src/               (AQUAHarborTools...)
  tests/
    unit/ integration/ perf/
```

## Governance
- Milestones:
- `0.1 Contract Reset`
- `0.2 Runtime + Render Slice`
- `0.3 Simulation Stack`
- `0.4 Studio Editor`
- `0.5 Multiplayer`
- `1.0 Production`
- Freeze APIs at each milestone and permit only additive/stabilizing changes inside freeze windows.
- Run monthly architecture review focused on:
- simplicity-vs-depth API quality
- naming compliance
- dependency hygiene and performance deltas
