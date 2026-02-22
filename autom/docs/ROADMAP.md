# AUTOM Roadmap

This roadmap defines a practical path to make AUTOM a complete build system generator.

## Definition of Complete

AUTOM is "complete" when it can:

- Model real-world projects with stable semantics (targets, dependencies, configs, install, custom commands).
- Generate reliable projects for Ninja, Visual Studio, and Xcode with equivalent behavior.
- Support host and cross builds across macOS/Linux/Windows with predictable toolchain behavior.
- Provide strong diagnostics, reproducible outputs, and CI-grade test coverage.

## Current Baseline (from code today)

- Strong: language evaluator, core target model (`Executable`, `Archive`, `Shared`, `SourceGroup`, `GroupTarget`, `Script`), Ninja generation, integration tests.
- Partial: Xcode generator exists and works for core flows; Visual Studio generator is still mostly a stub.
- Gaps to address first:
  - Toolchain loader platform selection is incomplete and fragile for non-macOS flows.
  - Generator parity across Ninja/VS/Xcode is not yet guaranteed.
  - Config model (`Debug/Release`) and transitive usage requirements are limited.
  - CLI/help exposes options not fully implemented (or inconsistently implemented).

## Guiding Principles

- Keep language semantics stable while improving generators.
- Add features behind tests; no feature lands without coverage.
- Prioritize deterministic output and clear error reporting over feature count.

## Milestone Plan

## M0 - Foundation Hardening (2-4 weeks)

Goal: stabilize internals so new features are safe to add.

### Scope

- Toolchain loader correctness:
  - Fix platform matching logic for `linux`, `android`, `ios`, and `windows`/`macos`.
  - Validate required `progs`/`flags` fields per host OS before use.
  - Improve errors to identify missing keys in toolchain JSON.
- Driver/CLI consistency:
  - Audit all documented flags and align behavior with help output.
  - Keep cleanup behavior deterministic and non-destructive (already started with build-files cleanup).
- Graph and evaluator safety:
  - Better diagnostics for invalid target properties and invalid builtin argument types.
  - Dependency validation improvements (cycle and duplicate target-name checks).

### Exit Criteria

- Toolchain matrix tests pass for supported platform selections.
- No crashes/assertions from malformed toolchain entries.
- All CLI flags in help are either implemented or removed.

## M1 - Generator Parity Core (4-8 weeks)

Goal: make Ninja/VS/Xcode equivalent for core C/C++ workflows.

### Scope

- Define canonical internal build model (single source of truth):
  - Target type, sources, deps, include dirs, libs/lib dirs, output name/dir/ext, scripts, install rules.
- Ninja parity cleanup:
  - Ensure all target properties map cleanly and consistently.
- Visual Studio generator completion:
  - Implement `.sln` + `.vcxproj` generation with target deps, include paths, libs, build configs.
- Xcode parity expansion:
  - Complete mapping for config-specific settings and dependency semantics.
- Add `compile_commands.json` generation as standard output option.

### Exit Criteria

- A shared golden project produces equivalent build graph intent in all three generators.
- VS and Xcode output can be loaded and built for core sample projects.
- Integration tests cover parity checks for all core target kinds.

## M2 - Configurations and Transitive Usage (4-6 weeks)

Goal: support production-style target configuration and dependency propagation.

### Scope

- First-class build configurations:
  - `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel`.
  - Per-config `cflags`, `ldflags`, defines, and output directories.
- Usage requirements model:
  - Public/private include dirs, defines, link deps, and propagation to dependents.
- Link model improvements:
  - Shared/static link semantics per platform.
  - Import library behavior and platform naming conventions.

### Exit Criteria

- Config-specific outputs and flags validated across generators.
- Transitive include/lib behavior validated by compile+link tests.

## M3 - Install, Packaging, and Reproducibility (4-6 weeks)

Goal: make AUTOM viable for distribution and CI release workflows.

### Scope

- Install/export enhancements:
  - Component-based installs, relocatable install rules, export metadata.
- Reproducibility:
  - Deterministic generator output ordering.
  - Environment/toolchain capture in generated metadata.
- Optional dependency workflow:
  - External dependency fetch/build hooks with lockfile (minimum viable).

### Exit Criteria

- Release pipeline can build, install, and package a multi-target project reproducibly.
- Repeated generation without source changes yields stable file diffs.

## M4 - Developer Experience and Scale (ongoing)

Goal: improve usability for larger projects and teams.

### Scope

- Diagnostics and traceability:
  - Explain mode (`why this target depends on X`, `why this flag was applied`).
  - Better source-mapped errors in `AUTOM.build`.
- Performance:
  - Build graph metrics, incremental generation speed improvements.
- IDE support:
  - Improved project sync ergonomics and run/debug metadata generation.

### Exit Criteria

- Large project generation latency targets met.
- Diagnostics reduce issue triage time in CI and local dev.

## Testing Strategy by Milestone

- M0: parser/evaluator negative tests, toolchain schema tests, crash-proofing tests.
- M1: generator golden tests (Ninja/VS/Xcode), smoke-build tests on each generator.
- M2: transitive behavior tests and config matrix compile/link tests.
- M3: install/package reproducibility tests (byte-for-byte or stable hash checks).
- M4: performance regression benchmarks and diagnostics snapshot tests.

## Suggested Execution Order (Pragmatic)

1. M0 toolchain + CLI hardening.
2. M1 Visual Studio completion and parity tests.
3. M2 config/transitive model.
4. M3 install/export/reproducibility.
5. M4 DX/performance improvements.

## Risks and Mitigations

- Risk: generator behavior divergence.
  - Mitigation: canonical build model + parity golden tests.
- Risk: platform-specific breakage.
  - Mitigation: CI matrix across macOS/Linux/Windows with minimal representative projects.
- Risk: language churn slowing adoption.
  - Mitigation: versioned policy/deprecation model after M1.

## Immediate Next Sprint (recommended)

- Implement M0 toolchain loader platform fixes and schema validation.
- Add explicit cycle/duplicate-target detection diagnostics.
- Add parity test fixture shared across Ninja/Xcode/VS.
- Define and document canonical internal build model fields.
