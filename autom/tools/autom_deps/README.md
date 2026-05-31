# autom-deps

`autom-deps` is the dependency manager for the Omega Graphics project. Whenever the source tree is missing a third-party library, an SDK archive, or a portable tool that the build expects to find under `deps/`, running this command brings the tree back into a state where the build can succeed. It is the single command an agent or a user should run when they suspect that something under `deps/` is missing, out of date, or freshly broken.

The launcher lives at `autom/tools/autom-deps` (a one-line shell script that forwards to `autom_deps/main.py`). The implementation lives in this directory and is pure Python; the only third-party Python package it needs is `requests`. There is no build step. If `python3` is on the path and `requests` is installed, the tool runs.

## What problem autom-deps solves

Every project under the Omega Graphics tree has its own list of third-party dependencies — libxml2 for WTK, RapidJSON for autom, the Vulkan SDK for GTE on Linux, Strawberry Perl for OpenSSL builds on Windows, the Android NDK for the Android target, and so on. These dependencies are not vendored into the repository. They are described in plain-text JSON manifests called `AUTOMDEPS`, one per project, and the manager walks those manifests and materialises whatever they describe into `deps/` directories on disk. Some dependencies are fetched as git checkouts so that we can patch and build them from source; others are fetched as release archives or one-off files because that is the only form the upstream publishes; a few are simply pointers to a folder that must already exist on disk (a system-installed SDK, for example).

The same tool also acts as a verifier and a cleaner. If a build fails because something is missing, the agent or the user runs `autom-deps` once and the tool fetches whatever is missing, leaves the rest alone, and reports which dependencies it touched. If a checkout has gone stale, `autom-deps --sync` pulls the latest commits on every git dependency. If a single dependency needs to be wiped and refetched from scratch, `--clean <name>` followed by a normal run does that without disturbing anything else.

## Running the tool

Run `autom-deps` from the project root — the directory that contains the top-level `AUTOMDEPS` file. The tool always looks for `AUTOMDEPS` in the current working directory and refuses to run if it cannot find one.

The default mode does the obvious thing: walk the manifest tree, materialise anything that is missing, and leave anything that is already in the right state alone.

    python3 autom/tools/autom_deps/main.py
    # or, equivalently, on a Unix-like shell:
    autom/tools/autom-deps

On Windows, run the `.bat` wrapper instead:

    autom\tools\autom-deps.bat

The tool detects your host platform automatically and only acts on dependencies marked for that platform. You can override the platform when you need to fetch dependencies for a different target — for example, to pre-fetch the Android NDK from a macOS host:

    autom/tools/autom-deps --target android

The supported target values are `windows`, `macos`, `linux`, `ios`, and `android`.

### The modes you will actually use

Most of the time you only need three modes: the default run (fetch what is missing), a sync (pull latest commits on git dependencies), and the occasional refresh (force one dependency to be refetched). The other modes exist for less common situations and are listed further down.

**Default (fetch missing).** Run `autom-deps` with no flags. Everything already on disk is reused. Anything missing is fetched. This is the right command when the build fails with a "file not found" error pointing at something under `deps/`.

**Sync (`--sync`).** Run `autom-deps --sync` to update every git checkout to the latest commit on whatever branch or ref the manifest pins. Archive and file dependencies are not touched. Use this when you want to bring third-party source up to date before a build.

**Refresh (`--refresh <name>` or `--refresh-all`).** Refresh forces autom-deps to refetch a specific dependency even if its on-disk copy looks fine. `--refresh-all` does the same for every dependency in the manifest. Use this when the on-disk copy is corrupted, was hand-edited and needs to be reset, or the manifest changed in a way that requires a fresh download.

**Verify (`--verify`).** Run `autom-deps --verify` to walk the manifests without changing anything on disk. The tool checks that every required artifact exists and (for archive/file dependencies that declare `sha256` or `size`) that the contents match. Verify is read-only and exits non-zero if anything is missing or fails its checksum.

**Dry-run (`--dry-run`).** Print the actions the tool would take without taking them. Useful for previewing what a fresh run will do.

**Clean (`--clean <name>`).** Remove the artifacts associated with one dependency — its destination directory, any cached archive, any extracted temporary directory. The dependency stays in the manifest; only its on-disk outputs are deleted. Follow a clean with a normal run to refetch from scratch. `--clean` may be passed multiple times to clean several dependencies in one invocation. Local dependencies (which are user-managed paths, not fetched by the tool) cannot be cleaned and will refuse.

### Output controls

`--verbose`, `--quiet`, and `--json-log` control how the progress is reported. `--verbose` adds extra detail; `--quiet` reduces output to errors only; `--json-log` emits one JSON event per line, which is the right choice when another tool is parsing the output. `--progress` and `--no-progress` force-enable or force-disable the progress bar regardless of the terminal type. `--single-stream` and `--no-resume` change download behaviour: by default downloads resume from where they left off if interrupted; `--no-resume` disables that and `--single-stream` forces a single download stream rather than parallel chunks.

### CMake integration

If you pass `--cmake <path>`, autom-deps writes a CMake include file at that path containing one `set()` line per exported value. The Omega Graphics build picks this file up so that CMake can refer to fetched dependencies by symbolic name — for example, the path to the Vulkan SDK or to the Strawberry Perl executable — without hard-coding paths in `CMakeLists.txt`. The variable names follow the pattern `<PROJECT>_<DEPENDENCY>_<KEY>`, all uppercase, with non-alphanumeric characters replaced by underscores. The file regenerates atomically on every run; do not edit it by hand.

### Reading resolved values from the command line

`--print-resolved <dependency>.<field>` prints a single resolved value from the state file and exits. This is useful in scripts that need to know, for example, the absolute path where a dependency landed. The dependency must have been run at least once so that the state file contains an entry for it.

## How the manifests work

The top-level manifest is `AUTOMDEPS` at the repository root. It declares the projects it owns and the subdirectories it should descend into. Each subdirectory listed under `subdirs` must contain its own `AUTOMDEPS` file. The tool walks the tree depth-first and processes each manifest in turn, so a single run at the root materialises everything for the whole repository.

A manifest is JSON with the following top-level keys, all optional:

- `project` — a human-readable name for the project. Used as a prefix when exporting values to CMake.
- `variables` — a flat object of named values that can be referenced elsewhere in the same manifest. A variable reference looks like `$(name)`. Variables may reference other variables; cycles are detected and reported as errors. Child manifests inherit the parent's variables and may add or override their own.
- `dependencies` — the list of third-party libraries, archives, files, tools, or local paths that this project needs.
- `commands` — an optional list of one-off actions to execute after the dependencies in this manifest are processed. These are the lower-level building blocks (download a URL, unzip an archive, clone a git repository, run a script) and are used when a project needs something beyond the standard dependency types.
- `rootCommands`, `postCommands`, `postRootCommands` — additional command hooks that run at specific points in the walk. `rootCommands` run before anything else and only fire on the top-level manifest. `postCommands` run after the dependencies of the current manifest. `postRootCommands` run at the very end of the walk and only on the top-level manifest.
- `subdirs` — a list of relative directories whose own `AUTOMDEPS` files should be processed after the current manifest. Each entry must point at a directory that contains its own `AUTOMDEPS`.

Validation runs over every manifest before any work happens. Unknown keys, missing required fields, type errors, duplicate dependency names across the whole tree, and undefined or cyclic variable references all produce a clear error and abort the run before anything is fetched.

### Dependency types

Each entry in the `dependencies` list has a `type` field that selects how the dependency is materialised. The supported types are `git`, `archive`, `file`, `tool`, and `local`.

**`git`** — clone a git repository. Required keys are `url` (the clone URL) and `dest` (the destination directory). An optional `ref` checks out a specific branch, tag, or commit after cloning. On a normal run, if the destination already exists and is a git checkout, the tool leaves it in place; with `--sync` or `--refresh`, the tool runs `git fetch` and either checks out the pinned ref or pulls the current branch. Most third-party libraries the project builds from source are git dependencies.

**`archive`** — download a zip or tarball and extract it. Required keys are `url` and `dest`. Optional keys include `archive_name` (the local filename to save the download as; inferred from the URL if omitted), `strip_components` (how many leading path components to strip during extraction, the same idea as `tar --strip-components`), `dest_subdir` (extract into a subdirectory of `dest`), `expected_root` (assert that the archive's top-level entry has this name), `rename_root_to` (rename the single top-level entry on extraction), and `sha256`/`size` (verify the download against an expected hash and/or byte count). Archive downloads are cached under `.automdeps/cache/<name>/`, so a re-run does not redownload an archive that has not changed.

**`file`** — download a single file (not extracted). Required keys are `url` and `dest`. Optional `sha256` and `size` verify the downloaded contents.

**`tool`** — wrap one of the other dependency types as a portable tool. The shape is `{"type": "tool", "source": { ... a nested git/archive/file/local definition ... }}`. The wrapping does not change how the underlying source is materialised, but it marks the dependency as a tool (used by Windows builds to fetch portable Strawberry Perl, for example) and gives it a single place to expose `exports` for the build system.

**`local`** — declare that the dependency is a path the user provides outside of autom-deps. Required key is `path`. The tool only checks that the path exists; it does not fetch anything. This is used when a dependency must be installed system-wide (a SDK, a compiler) and the project just needs CMake to know where to find it.

### Exports

Every dependency can declare an `exports` object whose values are paths (or path templates that reference variables). The tool resolves those paths against the manifest's directory, prefixes each key with the dependency's name, and records the result in `.automdeps/exports.json` and (if requested) in the generated CMake file. For example, the OmegaCommon manifest declares this for Strawberry Perl on Windows:

    "exports": {
        "perl": "$(third_party_dest)/strawberry-perl/perl/bin/perl.exe"
    }

After a run, that path is available to the build system as the exported value `strawberry-perl.perl`, which the CMake export writer turns into `OMEGACOMMON_STRAWBERRY_PERL_PERL`. The build system uses the exported variable rather than guessing where the tool landed.

Every dependency that has a `dest` (or, for `local`, a `path`) also gets an implicit `<name>.root` export pointing at that location. You do not need to declare it by hand.

### Conditional gating: `platforms` and `when`

Many dependencies are platform-specific. The Vulkan SDK is fetched on Linux only; the DirectX libraries are fetched on Windows only; the Android NDK has a separate URL for each host operating system. There are two ways to gate a dependency.

`platforms` is a simple allow-list of build targets:

    "platforms": ["windows"]

The dependency runs only when the resolved target (the host platform, or whatever `--target` selected) is in this list.

`when` is a richer condition object. It supports the keys `platform` (same idea as `platforms`, kept for readability when combined with other conditions), `arch` (a list of architectures such as `x86_64` or `arm64`), `host` (the actual host operating system, regardless of the build target — useful for picking the right NDK package), `exists` (a list of paths that must all exist on disk for the condition to hold), and `not_exists` (a list of paths that must all be absent). All conditions in a `when` block must hold for the dependency to run.

The Android NDK manifest at the repository root is the canonical example: there are three Android NDK entries, all targeting the `android` platform, gated on `host` so that exactly one of them runs depending on whether the user is on macOS, Linux, or Windows.

### Version sources

A dependency can opt into automatic version discovery by adding a `version_source` block. The tool queries GitHub (or another supported source) to find the latest stable release, fills in the relevant fields on the dependency, and records the resolved values in the state file so that future runs reuse them. The supported strategies are:

- `git-default-branch` — query GitHub for the repository's default branch and use it as the git `ref`.
- `git-tag-latest-stable` — pick the most recent non-pre-release tag and check it out.
- `git-branch-pattern` — derive a branch name from a template that references a major-version number, where the major-version comes from a `major_source` (typically the most recent GitHub release). The ICU dependency uses this to track the `maint/maint-<major>` branch corresponding to the latest stable major release.
- `github-releases` — pick the latest stable GitHub release and use either a templated asset name or the release's zipball/tarball as the download URL.
- `url-template` — pick the latest stable GitHub release and substitute its version and tag into a download URL template (used for projects that host their releases on a separate download server but cut versions in lockstep with their GitHub tags).
- `manual` — pin a specific value, recorded so that the build is reproducible.

Resolution happens lazily: once a dependency has been resolved, the value is cached in the state file and reused on the next run. Pass `--resolve-stable` to force a fresh resolution even when a cached value exists.

### Commands

Sometimes a project needs to run a one-off action that is not modelled as a dependency — clone a repository to a non-standard location, unzip a file that is already on disk, run a Python script as part of bootstrap. The `commands`, `rootCommands`, `postCommands`, and `postRootCommands` lists let a manifest declare those actions. The supported command types are:

- `git_clone` / `clone` — clone a git repository. The difference between the two is that `clone` adds the resulting directory to a queue of nested AUTOMDEPS manifests to walk after the current tree finishes, which is how third-party components that ship their own manifests get bootstrapped.
- `chdir` — change directory.
- `system` — run a shell command.
- `script` — run a Python script via `runpy`, with arguments resolved through the variable substitution mechanism.
- `download` — fetch a URL to a destination path.
- `tar` — extract a tarball that already exists on disk.
- `unzip` — extract a zipfile that already exists on disk.

All commands support the same `platforms` and `when` gating as dependencies.

## State, caches, and outputs

Every successful run writes a small amount of bookkeeping to `.automdeps/` at the project root:

- `.automdeps/state.json` — records every dependency and command that has run, what inputs it had, what it produced, and where on disk it landed. This file is what makes incremental runs fast: on the next invocation, the tool consults this file to see whether the on-disk state still matches what the manifest asks for, and skips anything that does.
- `.automdeps/exports.json` — the flat list of exported values, ready to be consumed by other tooling.
- `.automdeps/cache/<dependency>/` — downloaded archive originals. These are kept so that a refresh that does not change the URL can re-extract without redownloading.
- `.automdeps/tmp/<dependency>/` — scratch space used while extracting archives. The tool cleans these up after a successful extraction.

Treat the `.automdeps/` directory as a build artifact. It can be deleted safely and the next run will rebuild it. Do not check it into source control.

## When something goes wrong

The tool aims to fail loudly with a clear message. The most common situations:

- **"AUTOMDEPS file not found"** — you ran the tool from somewhere other than the repository root. `cd` to the directory that contains the top-level `AUTOMDEPS` and try again.
- **"destination exists and is not a git checkout"** — the manifest expects a git repository at a path that exists on disk but is not a git checkout. Inspect the directory, decide whether to keep it or remove it, and then re-run.
- **"sha256 mismatch" or "size mismatch"** — a downloaded artifact does not match the hash or size declared in the manifest. Either the upstream changed (in which case the manifest needs to be updated) or the download was corrupted (in which case `--refresh <name>` should be enough to retry).
- **"undefined variable" or "cyclic variable reference"** — a manifest references a variable that is not declared anywhere or that references itself through a chain. Fix the manifest.
- **"duplicate dependency name across manifests"** — two manifests in the tree declare the same dependency name. Names must be unique across the entire repository, not just within one manifest, because exports are keyed by name.

If a run dies partway through, you can usually just re-run with no flags. Anything that completed will be skipped; anything that did not will be retried. The download manager resumes interrupted downloads by default, so partial downloads do not need to start over.

## Worked example: adding a new dependency

Suppose the WTK project starts using a small header-only library called `acme-headers` that lives on GitHub at `https://github.com/example/acme-headers.git`. The steps are:

1. Open `wtk/AUTOMDEPS`.
2. Add a new entry to the `dependencies` list, using one of the existing entries as a template:

    ```
    {
        "name": "acme-headers",
        "type": "git",
        "url": "https://github.com/example/acme-headers.git",
        "dest": "$(third_party_dest)/acme-headers/code"
    }
    ```

3. From the repository root, run `autom-deps`. The tool walks the tree, notices the new dependency, clones it into `wtk/deps/acme-headers/code/`, and updates the state file. Re-running does nothing because the destination already exists and matches.

If the new dependency only matters on a specific platform, add a `platforms` list. If it only matters when something else is already present, add a `when.exists` clause. If it should be pinned to a specific release tag, add `"ref": "v1.2.3"`. If the build needs to know the include directory, add an `exports` block that points at it.

A new dependency typically needs no changes anywhere else: the build system reads `.automdeps/exports.json` (or the generated CMake file) to discover where the dependency landed.

## How autom-deps fits with the rest of the toolchain

`autom-deps` is the first thing that runs in a fresh checkout. It populates `deps/` directories under each project so that the CMake/Ninja build (driven separately) can find everything it needs. It does not invoke the compiler, it does not configure CMake, and it does not understand which targets are buildable on the current host. Those concerns belong to the build system; `autom-deps` is concerned only with making sure the inputs to the build exist on disk and are in a known-good state.

When `AGENTS.md` says "If a dependency is missing from a repo tree, rerun autom-deps," this is the command it means. There is no separate "install" step, no package lockfile to refresh, and no virtual environment to activate. Run the command, let it work, and the tree is ready for the build.
