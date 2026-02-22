# AUTOM Test Suite Design

This suite is structured to validate three things independently:

1. Language/evaluation correctness.
2. Build graph generation correctness across toolchains and generators.
3. Real compile+link+runtime behavior on the host.

## Test Tiers

| Tier | Scope | Requires real compiler/linker | Primary signal |
|---|---|---:|---|
| T0 | Language semantics + builtin behavior | No | Parse/eval output and diagnostics |
| T1 | Generator correctness (`ninja`, `xcode`, `sln`) | No | Generated files and rule contents |
| T2 | Host end-to-end compile/link/run | Yes | `ninja` success + process exit code |
| T3 | Extensions/modules integration (`fs`, imports, subdir) | Usually yes | Generated sources, runtime exit code |

## Toolchain Matrix

The matrix is validated in generation mode by injecting shim tools into `PATH` and supplying a per-case toolchain file.

| Case | Target Platform/OS | Toolchain | Assertions |
|---|---|---|---|
| GNU Linux | `linux/linux` | GCC | `gcc/g++/ld/ar` command wiring, `.a/.so`, executable rule |
| LLVM Linux | `linux/linux` | LLVM | `clang/clang++/ld.lld/llvm-ar` command wiring, `.a/.so` |
| LLVM Windows | `windows/windows` | LLVM | `clang-cl/lld-link/llvm-lib` command wiring, `.lib/.dll/.exe` |
| MSVC Windows | `windows/windows` | MSVC | `cl/link/lib` command wiring, `.lib/.dll/.exe` |

## Feature Coverage Map

| Area | Covered by current tests |
|---|---|
| Core language (`var`, `func`, `if/elif/else`, `foreach`, array concat) | Yes |
| Builtins (`project`, target creation, `install_*`, `subdir`, `find_program`, `config_file`) | Yes |
| Target types (`Executable`, `Archive`, `Shared`, `SourceGroup`, `GroupTarget`, `Script`) | Yes |
| Generator modes (`--ninja`, `--xcode`, `--sln`) | Yes (smoke/integration) |
| Toolchain-specific compile/link command formatting | Yes (matrix generation tests) |
| Runtime binaries | Yes (host-only integration tests) |

## Edge Cases Covered

- Unresolved dependencies block generation.
- Targets with empty `sources` are rejected.
- `Script` target without outputs is rejected.
- Missing interface imports fail fast.
- Target flag/output overrides propagate into generated Ninja rules.

## CI Profiles

- Fast PR profile:
  - Run T0 + T1 only (language + generator + matrix generation).
  - No dependence on host compiler ecosystem.
- Full profile:
  - Run all tiers including T2/T3 runtime builds.
  - Suitable for nightly/release pipelines.

## Follow-up Expansions

- Dedicated lexer/parser unit tests (token-level and AST-level golden tests).
- Negative tests for argument type mismatches and invalid property access diagnostics.
- Platform-specific runtime lanes (Windows runner for true MSVC link/exec behavior).
