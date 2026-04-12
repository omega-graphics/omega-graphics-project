# `autom-deps` Completion Plan

## Purpose

`autom-deps` currently works as a lightweight fetch/bootstrap helper for project-local
third-party dependencies declared in `AUTOMDEPS` JSON files. It is already useful for
simple cases, but it stops short of being a dependable project dependency manager.

This plan proposes a focused extension that turns `autom-deps` from a small command
runner into a reproducible, platform-aware dependency bootstrap layer that can be relied
on by the Omega Graphics Project and other AUTOM-based repositories.

The goal is not to replace system package managers or CMake's `ExternalProject`.
The goal is to finish `autom-deps` as the layer that:

1. Fetches source archives, tool runtimes, and vendored repositories
2. Verifies what was fetched
3. Extracts/stages those artifacts predictably
4. Exposes resolved paths to build systems such as CMake and AUTOM
5. Supports updates and re-runs without becoming brittle

## Current State

Today `autom-deps` supports a small command vocabulary in
[`autom/tools/autom_deps.py`](../tools/autom_deps.py):

| Command | Status |
|--------|--------|
| `git_clone` | Implemented |
| `clone` | Implemented |
| `download` | Implemented |
| `tar` | Implemented |
| `unzip` | Implemented |
| `script` | Implemented |
| `system` | Implemented |
| `chdir` | Implemented |

It also supports:

- recursive `subdirs`
- `rootCommands`
- `postCommands`
- simple `platforms` filters
- string variable substitution via `$(name)`

This is enough for simple "download and extract" cases, but there are major gaps:

1. No integrity verification for downloads
2. No idempotent skip logic for already-fetched artifacts
3. No explicit support for archive layouts that unpack into versioned directories
4. No standardized way to export discovered paths to downstream build steps
5. No manifest validation or schema enforcement
6. No structured logging, status output, or dry-run mode
7. No robust update semantics beyond `git pull`
8. No lockfile/state file describing what was fetched and from where
9. No tool bootstrap model for runtimes such as Perl, Python, CMake, Ninja, or SDK tools
10. No first-class conditional model beyond a flat `platforms` list

## Problem Statement

The current `AUTOMDEPS` format is strong enough to describe a sequence of imperative
actions, but weak at describing dependency intent.

That causes recurring issues:

- projects duplicate the same fetch/extract patterns
- version bumps require editing multiple URL/path strings by hand
- downstream build files need to guess where extracted content landed
- a partial failed run may leave the tree in an ambiguous state
- a successful run is not reproducible unless every remote still serves identical bytes

The recent Strawberry Perl case is a good example: the project needs a Windows-only
runtime tool in order to configure OpenSSL, but `autom-deps` currently models that as a
best-effort URL plus unzip step, rather than as a first-class provisioned tool.

## Design Goal

Finish `autom-deps` by introducing a declarative layer on top of the existing command
runner, while preserving backward compatibility for current `AUTOMDEPS` files.

That means:

- old command-based manifests continue to run
- new manifests can opt into richer dependency records
- the executor becomes stateful, validated, and idempotent
- the output of `autom-deps` becomes consumable by CMake, AUTOM, and shell scripts

## Proposed Extension: `autom-deps v2`

The extension is a new manifest model with three first-class sections:

1. `variables` for reusable values
2. `dependencies` for declarative fetched/staged artifacts
3. `commands` for imperative fallback/custom steps

Example:

```json
{
  "variables": {
    "third_party_dest": "./deps",
    "openssl_version": "3.5.0",
    "strawberry_perl_version": "5.40.2.2"
  },
  "dependencies": [
    {
      "name": "openssl-src",
      "type": "git",
      "url": "https://github.com/openssl/openssl.git",
      "dest": "$(third_party_dest)/openssl/code",
      "ref": "openssl-3.5.0"
    },
    {
      "name": "strawberry-perl",
      "type": "archive",
      "platforms": ["windows"],
      "url": "https://sourceforge.net/projects/perl-dist-strawberry.mirror/files/SP_54022_64bit/strawberry-perl-5.40.2.2-64bit-portable.zip/download",
      "sha256": "<expected sha256>",
      "archive_name": "strawberry-perl.zip",
      "strip_components": 0,
      "dest": "$(third_party_dest)/tools/strawberry-perl",
      "exports": {
        "root": "$(third_party_dest)/tools/strawberry-perl",
        "perl": "$(third_party_dest)/tools/strawberry-perl/perl/bin/perl.exe"
      }
    }
  ],
  "commands": [
    {
      "type": "script",
      "path": "./tools/post_fetch.py",
      "args": ["$(third_party_dest)"]
    }
  ]
}
```

The major change is that `dependencies` become the preferred path for standard fetch and
staging work. `commands` remain available for unusual project-specific behavior.

## New Manifest Features

### 1. First-Class Dependency Records

Introduce dependency entry types:

| Type | Purpose |
|------|---------|
| `git` | clone/fetch a repository at a ref/branch/tag/commit |
| `archive` | download and extract zip/tar artifacts |
| `file` | download a single file with checksum validation |
| `tool` | provision a runtime/tool and expose exported paths |
| `local` | register existing local paths into the export graph |

`tool` is conceptually the same as `archive` or `git`, but documents that the dependency
exists to provide an executable/runtime rather than source code. That matters for path
exports and downstream reporting.

### 2. Validation and Schema

Before executing anything, `autom-deps` should validate the manifest:

- required fields for each entry type
- unknown keys rejected
- variables checked for cycles
- dependency names validated as unique
- exported path keys validated as unique
- platform filters validated against the supported set

This can be implemented with a small internal schema validator instead of pulling in a
large validation framework.

### 3. Recursive Variable Resolution

Variable substitution should fully support:

- nested references, e.g. `$(archive_name)` containing `$(version)`
- bounded recursion with cycle detection
- substitution in strings, arrays, and object string values

This closes the current class of nested-variable bugs and makes manifests less fragile.

### 4. Checksums and Integrity

`download` and `archive` dependencies should support:

- `sha256`
- optional `size`
- optional `final_url` recording in state output

If a checksum is provided, the tool must verify it before extraction. A mismatch should
abort the run.

This is required before `autom-deps` can be treated as a trustworthy bootstrap layer for
toolchains and crypto-related dependencies.

### 5. Idempotent State File

Add a generated state file, for example:

```text
.automdeps/state.json
```

Each dependency entry records:

- name
- source URL
- resolved ref/version
- checksum
- destination path
- extraction root
- timestamp
- last successful status

On re-run, `autom-deps` uses the state file plus on-disk checks to skip already-satisfied
dependencies unless explicitly told to refresh.

### 6. Export File for Build Integration

Generate a second output file:

```text
.automdeps/exports.json
```

Example:

```json
{
  "strawberry-perl.root": "/abs/path/common/deps/tools/strawberry-perl",
  "strawberry-perl.perl": "/abs/path/common/deps/tools/strawberry-perl/perl/bin/perl.exe",
  "openssl-src.root": "/abs/path/common/deps/openssl/code"
}
```

This export file becomes the contract between `autom-deps` and build systems.

Possible follow-on helpers:

- `autom-deps --print-export strawberry-perl.perl`
- `autom-deps --cmake exports.cmake`
- `autom-deps --shell exports.sh`

That allows CMake to consume exact paths without re-encoding layout guesses.

### 7. Archive Layout Control

Support the common archive extraction issues directly:

- `strip_components`
- `dest_subdir`
- `expected_root`
- `rename_root_to`

This avoids the repeated pattern where downstream CMake has to guess whether extraction
produced `foo/`, `foo-1.2.3/`, or an archive root with many top-level files.

### 8. Conditional Execution Model

Replace the current flat `platforms` model with richer conditions:

```json
"when": {
  "platform": ["windows"],
  "arch": ["x86_64", "arm64"],
  "exists": ["./deps/openssl/code"],
  "not_exists": ["./deps/tools/strawberry-perl/perl/bin/perl.exe"]
}
```

`platforms` can remain as a backward-compatible alias for the simple case.

### 9. Latest Stable Resolution

`autom-deps` should be able to resolve the latest stable version of a dependency without
forcing every project to hardcode the current branch, tag, or archive URL by hand.

This must work for both git-backed and archive-backed dependencies.

Proposed manifest shape:

```json
{
  "name": "icu",
  "type": "git",
  "url": "https://github.com/unicode-org/icu.git",
  "version_source": {
    "channel": "stable",
    "strategy": "git-branch-pattern",
    "pattern": "maint/maint-{major}",
    "major_source": {
      "type": "github-releases",
      "repo": "unicode-org/icu"
    }
  }
}
```

```json
{
  "name": "openssl-src",
  "type": "git",
  "url": "https://github.com/openssl/openssl.git",
  "version_source": {
    "channel": "stable",
    "strategy": "git-default-branch"
  }
}
```

```json
{
  "name": "pcre2",
  "type": "archive",
  "version_source": {
    "channel": "stable",
    "strategy": "github-releases",
    "repo": "PCRE2Project/pcre2",
    "asset_template": "pcre2-{version}.zip"
  }
}
```

Core ideas:

- `channel: "stable"` means "latest non-prerelease, non-nightly release line"
- the resolution strategy is explicit and depends on the upstream project's release model
- the resolved version/ref/url is written to the state file so builds remain reproducible
- users can pin the resolved output later if they need strict repeatability

Supported stable-resolution strategies should include:

| Strategy | Use case |
|----------|----------|
| `git-default-branch` | projects where the stable line is just the repo's main/default branch |
| `git-tag-latest-stable` | projects that publish stable tags and want the newest stable tag |
| `git-branch-pattern` | projects like ICU where the stable line is a versioned maintenance branch |
| `github-releases` | GitHub archive releases such as PCRE2 |
| `url-template` | non-GitHub archives where the latest stable version can be inserted into a known URL template |
| `manual` | fallback for projects that cannot be resolved automatically |

For ICU specifically, the resolver should:

1. query the latest stable upstream release
2. derive the major version, such as `78`
3. map that to `maint/maint-78`
4. clone or refresh that branch

For archive dependencies such as PCRE2, the resolver should:

1. query the latest stable release version
2. select the matching asset using `asset_template` or an asset filter
3. record both the resolved version and final download URL in state

This feature should also add CLI support for visibility:

```text
autom-deps --resolve-stable
autom-deps --print-resolved pcre2.version
autom-deps --print-resolved icu.ref
```

Important constraint: stable resolution must happen before download/clone execution, but
the resolved output must be cached in `.automdeps/state.json` so repeated runs stay
deterministic unless the user explicitly refreshes.

### 10. Toolchain-Aware Fetch Modes

Add dedicated update/fetch modes:

| Mode | Behavior |
|------|----------|
| default | fetch missing dependencies only |
| `--refresh name` | re-fetch one dependency |
| `--refresh-all` | re-fetch everything |
| `--verify` | verify checksums/state without fetching |
| `--dry-run` | print actions without mutating |
| `--clean name` | remove one dependency's staged outputs |

This makes the tool practical for iterative development instead of forcing users to
manually clean directories.

### 11. Structured Logging

Execution output should be readable and stable:

- one line announcing each dependency
- one line for skip/fetch/verify/extract/install results
- failure messages that include dependency name and phase

Optional:

- `--json-log` for machine-readable CI output

### 12. Rich Command-Line Output

Beyond structured logs, `autom-deps` should have a more polished and informative
interactive command-line presentation for human users.

The current output is functional, but too plain for large dependency runs. It becomes
hard to see:

- which dependency is currently active
- which phase is running
- whether a dependency was skipped, refreshed, verified, or failed
- what version/ref/url was ultimately chosen
- how far along a long download or extraction step is

Proposed output improvements:

- a clear session header showing root path, target platform, and selected mode
- per-dependency sections with a stable visual format
- phase labels such as `resolve`, `download`, `verify`, `extract`, `clone`, `refresh`,
  `export`, `done`
- aligned status markers such as `SKIP`, `FETCH`, `VERIFY`, `DONE`, `FAIL`
- indentation for sub-steps so nested work reads clearly
- byte counters and percentages for large downloads when content length is known
- final summary block with counts for completed, skipped, refreshed, and failed items

Example style:

```text
autom-deps :: OmegaCommon
root     : /repo/common
target   : windows
mode     : fetch

[FETCH] strawberry-perl
  resolve : stable archive -> 5.40.2.2
  url     : https://downloads.sourceforge.net/project/...
  file    : ./temp/strawberry-perl-5.40.2.2-64bit-portable.zip
  bytes   : 128 MiB / 302 MiB (42%)
  extract : ./deps/strawberry-perl
  done    : perl.exe exported

[SKIP ] openssl-src
  reason  : destination already present and state matches

Summary
  done    : 3
  skipped : 2
  failed  : 0
```

CLI switches should support both interactive and CI-friendly modes:

| Flag | Behavior |
|------|----------|
| `--verbose` | print all dependency phases and resolved metadata |
| `--quiet` | only print warnings, errors, and final summary |
| `--progress` | force progress display for downloads/extracts |
| `--no-progress` | disable dynamic progress output |
| `--color` | force ANSI color output |
| `--no-color` | disable ANSI color output |
| `--json-log` | emit machine-readable events instead of pretty terminal output |

Formatting rules:

- pretty output should be deterministic in wording so it is still grep-friendly
- dynamic progress lines should degrade cleanly when stdout is not a TTY
- color should be additive only, never required for meaning
- the same dependency name and phase terms should be used in both pretty and JSON modes

This feature is not just cosmetic. Better output reduces debugging time during bootstrap
failures, makes long-running dependency provisioning easier to trust, and makes
`autom-deps` feel like a finished tool rather than a thin script wrapper.

### 13. High-Performance Download Engine

Large dependency artifacts should download efficiently. The current simple HTTP fetch
model is correct enough for small files, but it is not a good long-term fit for large
SDK archives, portable runtimes, or source bundles in the hundreds of megabytes.

The download layer should be upgraded with performance as a first-class concern.

Target improvements:

- stream directly to disk without buffering large responses in memory
- use larger adaptive chunk sizes for high-throughput transfers
- reuse HTTP sessions across multiple downloads
- support resumable downloads when the remote supports range requests
- optionally download with multiple ranges in parallel for very large files
- persist partial files safely and verify them before resume
- avoid re-downloading already-matching files when checksum/state confirms validity

Recommended implementation model:

1. introduce a dedicated `DownloadManager` instead of inline `requests.get(...)`
2. reuse a single `requests.Session` for the whole run
3. detect `Accept-Ranges` and content length
4. for large files above a threshold, support segmented range downloads into temporary
   part files
5. merge parts atomically and verify checksum before finalizing

Suggested behavior tiers:

| File size / server capability | Behavior |
|------------------------------|----------|
| small file | single-stream download |
| large file, no range support | single-stream download with larger chunks |
| large file, range support | resumable segmented download |
| partially present file | verify and resume instead of restarting |

Suggested CLI controls:

| Flag | Behavior |
|------|----------|
| `--jobs N` | max concurrent downloads or segments |
| `--segment-size SIZE` | minimum part size for segmented download |
| `--no-resume` | disable resume logic |
| `--single-stream` | force a single HTTP stream |

Important constraints:

- correctness still comes first; checksum verification remains mandatory when configured
- segmented downloads must fall back cleanly when a server or mirror behaves poorly
- partial files must never overwrite a valid completed artifact
- progress reporting should reflect both total bytes and per-segment activity when useful

This feature pairs directly with the richer CLI output work. Faster downloads matter
most when users can also see that the tool is making real progress.

### 14. Split `autom_deps.py` into a Real Package

`autom/tools/autom_deps.py` is currently doing too many jobs in one file:

- CLI parsing
- manifest loading
- variable substitution
- conditional execution
- command execution
- download logic
- archive extraction
- recursive subdir traversal
- stateful global bookkeeping

That makes the tool harder to extend safely. The planned features in this document
already justify turning it into a small package rather than continuing to grow a single
script.

Proposed layout:

```text
autom/tools/autom_deps/
  __init__.py
  main.py
  cli.py
  manifest.py
  variables.py
  conditions.py
  commands.py
  downloads.py
  archives.py
  git_ops.py
  executor.py
  state.py
  ui.py
```

Suggested responsibility split:

| File | Responsibility |
|------|----------------|
| `main.py` | thin entry point that wires the package together |
| `cli.py` | argument parsing and mode selection |
| `manifest.py` | JSON load, validation, subdir traversal |
| `variables.py` | nested variable resolution and cycle handling |
| `conditions.py` | `platforms` / `when` filtering |
| `commands.py` | legacy command execution compatibility layer |
| `downloads.py` | `DownloadManager`, resume, segmented download, checksum hooks |
| `archives.py` | zip/tar extraction and archive layout normalization |
| `git_ops.py` | clone, fetch, checkout, stable-ref resolution helpers |
| `executor.py` | top-level planning and execution orchestration |
| `state.py` | `.automdeps/state.json` and exports handling |
| `ui.py` | pretty terminal output, progress display, JSON log formatting |

Entry-point changes:

- `autom/tools/autom-deps` should call `python3 "$(dirname -- "$0")/autom_deps/main.py" "$@"`
- `autom/tools/autom-deps.bat` should call `py -3 %~dp0autom_deps\\main.py %*`
- `autom/tools/clone.py` should import from the package, for example
  `from autom_deps.main import main`

Design constraints:

- preserve the current external CLI name: `autom-deps`
- preserve backward compatibility for existing `AUTOMDEPS` manifests
- keep the package importable from the `autom/tools` directory without requiring install
- move global mutable state behind explicit context objects where practical

Recommended refactor order:

1. create `autom/tools/autom_deps/main.py` as a compatibility entry point
2. move CLI parsing and manifest loading first
3. extract download/archive/git code into dedicated modules
4. move remaining execution logic behind an `ExecutionContext`
5. update shell and batch wrappers only after the package entry point is working
6. leave a temporary compatibility shim if needed during the transition

This refactor is important for maintenance, not just cleanliness. Without it, the
planned resolver, state, download, and UI improvements will all pile into a single file
and become increasingly risky to change.

### 15. Integration with `AUTOM.build`, CMake, and `autom-clone`

`autom-deps` should remain a separate tool. The important integration point is not a new
main-`autom` subcommand, but how dependency bootstrap results become consumable by
`AUTOM.build`, CMake, and `autom-clone`.

#### Goals

- keep `autom-deps` as a standalone bootstrap/provisioning tool
- make resolved dependency outputs consumable by `AUTOM.build`
- make resolved dependency outputs consumable by CMake
- keep `autom-clone` as the "clone and prepare" workflow
- avoid duplicating dependency resolution logic across multiple scripts

#### `autom-clone` Integration

Today `autom-clone` clones a repo and then directly invokes the dependency bootstrap if
an `AUTOMDEPS` file exists. That is the right high-level behavior, but it should be made
more explicit and configurable.

Planned behavior:

- `autom-clone <repo> <dest>` clones the repository
- if `AUTOMDEPS` exists, it invokes the shared `autom_deps` package entry point
- the clone command should surface dependency mode flags rather than hardcoding defaults

Suggested flags:

| Flag | Behavior |
|------|----------|
| `--deps` | run dependency bootstrap after clone |
| `--no-deps` | skip dependency bootstrap even if `AUTOMDEPS` exists |
| `--deps-verify` | clone, then verify/bootstrap dependencies |
| `--deps-refresh` | clone, then refresh dependencies |
| `--target <platform>` | forward target platform to `autom-deps` |

Recommended default:

- keep current behavior by treating `--deps` as the default unless `--no-deps` is given

#### `AUTOM.build` Integration

This is the more important integration target.

Today AUTOM has an `ExternalProject` concept in [`autom/modules/external_project.autom`](../modules/external_project.autom),
but that is still build-graph oriented. It fetches/configures/builds within the build
flow. `autom-deps` should instead provide a bootstrap/provisioning layer whose results
can be referenced from `AUTOM.build`.

That implies a contract between `autom-deps` and AUTOM:

- `autom-deps` resolves, fetches, verifies, and stages dependencies
- `autom-deps` writes `.automdeps/exports.json`
- `AUTOM.build` code reads exported paths/metadata and uses them as inputs
- build generation remains separate from dependency provisioning

Suggested AUTOM-side integration mechanisms:

1. add a small helper module, for example `autom_deps.autom` or an extension in
   `fs.autom`, that can read `.automdeps/exports.json`
2. expose lookups such as:

```autom
var openssl_root = AutomDepsExport(name:"openssl-src.root")
var perl_exe = AutomDepsExport(name:"strawberry-perl.perl")
var pcre2_include = AutomDepsExport(name:"pcre2.include")
```

3. optionally expose a bulk read form:

```autom
var deps = AutomDepsExports()
var openssl_root = deps["openssl-src.root"]
```

4. allow build files to fail clearly if an expected export is missing

This is the key separation:

- `autom-deps` provisions and describes dependency outputs
- `AUTOM.build` consumes those outputs, but does not own the provisioning logic

That model is preferable to forcing every project to encode fetch/extract/version logic
directly in `ExternalProject`.

#### CMake Integration

CMake should consume the same resolved dependency contract instead of re-deriving paths
or repeating upstream version logic.

Suggested outputs from `autom-deps`:

```text
.automdeps/exports.json
.automdeps/exports.cmake
```

The CMake export file should contain resolved variables such as:

```cmake
set(AUTOM_DEPS_OPENSSL_SRC_ROOT "/abs/path/common/deps/openssl/code")
set(AUTOM_DEPS_STRAWBERRY_PERL_PERL "/abs/path/common/deps/strawberry-perl/perl/bin/perl.exe")
set(AUTOM_DEPS_PCRE2_INCLUDE "/abs/path/common/deps/pcre2/code/pcre2-10.47/include")
```

Then CMake projects can do:

```cmake
include("${CMAKE_CURRENT_SOURCE_DIR}/.automdeps/exports.cmake" OPTIONAL)
```

or parse `exports.json` if they need richer metadata.

This gives both build systems the same dependency truth source:

- no duplicated release selection logic
- no duplicated archive-layout guessing
- no duplicated tool-runtime discovery logic

#### Shared Runtime and Config

`autom-deps`, `AUTOM.build` helpers, CMake glue, and `autom-clone` should agree on:

- target platform naming
- root directory detection
- terminal formatting conventions
- exit code semantics
- location of `.automdeps/state.json` and `.automdeps/exports.json`

If feasible, a small shared config contract should exist so future tools can discover:

- whether dependency bootstrap has already completed
- which exports are available
- whether the current repo uses `AUTOMDEPS`

#### Wrapper/Entry Point Relationship

After the package split, the preferred relationship should be:

- `autom/tools/autom-deps` and `.bat` wrappers call `autom_deps/main.py`
- `autom/tools/autom-clone` and `.bat` wrappers call `clone.py`
- `clone.py` imports and invokes the shared `autom_deps` package entry point

Direct package invocation is preferred over duplicating logic in shell/batch wrappers.

#### UX Expectations

Integration should make setup feel coherent:

1. `autom-clone` clones and prepares a repo
2. `autom-deps` can be rerun later to verify, refresh, or inspect dependency state
3. generated exports become available to both `AUTOM.build` and CMake configuration
4. failures clearly indicate whether the problem is clone-related, dependency-related, or
   build-generation-related

This is important for new users. If dependency bootstrap remains a separate hidden tool,
the setup story stays fragmented.

## Backward Compatibility

Backward compatibility is required.

The migration strategy:

1. Existing `commands`-only `AUTOMDEPS` files continue to work unchanged
2. Existing command names keep their current behavior
3. New manifests can add `dependencies` gradually
4. Internally, some command implementations can be rewritten to share code with the new
   dependency executor

That allows OmegaCommon, OmegaWTK, and other modules to migrate incrementally instead of
requiring a single repo-wide conversion.

## Proposed Internal Architecture

Split `autom_deps.py` into clear layers:

### Layer 1: Manifest Loader

Responsibilities:

- load JSON
- merge root/subdir manifests
- resolve variables
- validate schema

### Layer 2: Execution Planner

Turns manifest entries into a sequence of operations:

- fetch
- verify
- extract
- rename
- export
- run imperative commands

This is where dry-run and skip logic should live.

### Layer 3: Executors

One executor per dependency type:

- `GitDependencyExecutor`
- `ArchiveDependencyExecutor`
- `FileDependencyExecutor`
- `ToolDependencyExecutor`

And a separate compatibility executor for legacy command entries.

### Layer 4: State/Export Writers

Write:

- `.automdeps/state.json`
- `.automdeps/exports.json`

These files should be regenerated only on successful completion.

## Proposed CLI Additions

Current CLI:

```text
autom-deps [--sync] [--target windows|macos|linux|ios|android]
```

Extended CLI:

```text
autom-deps
autom-deps --target windows
autom-deps --dry-run
autom-deps --verify
autom-deps --refresh strawberry-perl
autom-deps --refresh-all
autom-deps --print-export strawberry-perl.perl
autom-deps --cmake .automdeps/exports.cmake
autom-deps --shell .automdeps/exports.sh
autom-deps --clean strawberry-perl
```

## Example: OmegaCommon with the Proposed Model

`common/AUTOMDEPS` could eventually become:

```json
{
  "variables": {
    "third_party_dest": "./deps",
    "pcre2_version": "10.47",
    "strawberry_perl_version": "5.40.2.2"
  },
  "dependencies": [
    {
      "name": "pcre2",
      "type": "archive",
      "url": "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.47/pcre2-10.47.zip",
      "dest": "$(third_party_dest)/pcre2/code",
      "expected_root": "pcre2-10.47",
      "exports": {
        "root": "$(third_party_dest)/pcre2/code/pcre2-10.47"
      }
    },
    {
      "name": "openssl-src",
      "type": "git",
      "url": "https://github.com/openssl/openssl.git",
      "ref": "openssl-3.5.0",
      "dest": "$(third_party_dest)/openssl/code",
      "exports": {
        "root": "$(third_party_dest)/openssl/code"
      }
    },
    {
      "name": "strawberry-perl",
      "type": "tool",
      "platforms": ["windows"],
      "source": {
        "type": "archive",
        "url": "https://sourceforge.net/projects/perl-dist-strawberry.mirror/files/SP_54022_64bit/strawberry-perl-5.40.2.2-64bit-portable.zip/download",
        "dest": "$(third_party_dest)/tools/strawberry-perl"
      },
      "exports": {
        "perl": "$(third_party_dest)/tools/strawberry-perl/perl/bin/perl.exe"
      }
    }
  ]
}
```

Then CMake could read:

- `strawberry-perl.perl`
- `openssl-src.root`

instead of guessing archive layouts itself.

## Implementation Phases

## Phase 1: Stabilize the Existing Engine

Scope:

- begin the `autom_deps.py` package split with a compatibility entry point
- keep current command format
- improve variable resolution
- add manifest validation for existing command entries
- add cleaner error reporting

Deliverables:

- `autom/tools/autom_deps/main.py` compatibility entry point
- nested/cycle-safe variable resolution
- validation errors with file/field context
- standardized status output

This phase is low risk and directly improves current manifests.

## Phase 2: Introduce `dependencies`

Scope:

- add `git`, `archive`, `file`, `tool`, and `local` dependency entry types
- build the execution planner
- add the richer conditional execution model
- preserve `commands` compatibility

Deliverables:

- dependency executor abstraction
- first-class dependency records
- state file
- export file
- archive layout controls
- conditional dependency evaluation

This is the phase that effectively "finishes" `autom-deps` as a dependency manager.

## Phase 3: Reproducibility and Verification

Scope:

- checksum verification
- stable-version resolution
- toolchain-aware fetch/refresh modes
- idempotent skip/refresh behavior backed by state
- `--verify`
- `--dry-run`
- `--refresh`
- `--refresh-all`
- `--clean`

Deliverables:

- trustworthy CI/bootstrap runs
- safer tool/runtime provisioning
- resolved latest-stable refs and versions recorded in state
- refresh/verify/clean workflows with deterministic behavior

## Phase 4: Build-System Integration

Scope:

- `--print-export`
- `--cmake`
- `--shell`
- AUTOM-side helper to read exported paths/metadata
- CMake-side consumption via generated `exports.cmake`
- `autom-clone` flag forwarding to dependency bootstrap

Deliverables:

- direct CMake consumption without path guessing
- cleaner `AUTOM.build` files with exported dependency lookups
- integrated dependency workflows through `autom-deps` and `autom-clone`

## Phase 5: UX, Performance, and Tool Refactor

Scope:

- rich command-line output
- structured logging polish
- high-performance download engine
- full `autom_deps.py` package split into `autom/tools/autom_deps/`
- shared UI/state/download helpers across wrappers

Deliverables:

- verbose and pretty terminal output with progress reporting
- CI-friendly quiet/JSON logging modes
- faster and resumable large-file downloads
- maintainable multi-file `autom_deps` package layout
- updated wrappers targeting `autom_deps/main.py`

## Phase 6: Migrate Omega Modules

Pilot candidates:

1. `common/AUTOMDEPS` — good first candidate because it has an archive, a git source,
   and a Windows-only tool bootstrap
2. `wtk/AUTOMDEPS` — exercises multiple git dependencies
3. root `AUTOMDEPS` — validates recursive/subdir behavior

Migration should happen one module at a time, not in a single bulk rewrite.

## Testing Plan

### Unit-Level

- variable expansion
- cycle detection
- condition evaluation
- manifest validation
- checksum verification
- state-file skip logic

### Integration-Level

- archive fetch/extract into temp directories
- git clone and refresh
- Windows-only dependency skipped on Linux
- export generation for nested subdir manifests

### Repository-Level

- run against root `AUTOMDEPS`
- run against `common/AUTOMDEPS`
- verify that repeat runs become mostly no-op

## Non-Goals

This proposal does not attempt to turn `autom-deps` into:

- a full package manager like `vcpkg`, `Conan`, `Homebrew`, or `apt`
- a build graph generator
- a replacement for CMake's actual configure/build/install steps
- a binary package cache service

Those are separate concerns.

## Recommended First Slice

If only one extension is implemented next, it should be:

1. add first-class `archive`/`git`/`tool` dependencies
2. add checksum verification
3. add `.automdeps/exports.json`

That single slice would solve the biggest current weakness: `autom-deps` can fetch
things, but it still cannot reliably describe and export what it fetched.

## Summary

The completion path for `autom-deps` is not "more command types."

The real missing piece is a declarative dependency layer with:

- validation
- reproducibility
- exported resolved paths
- idempotent execution
- backward compatibility

Once those pieces exist, `autom-deps` becomes the correct place to bootstrap vendored
source trees and auxiliary tool runtimes for Omega projects, including Windows-specific
tooling such as Strawberry Perl for OpenSSL configuration.
