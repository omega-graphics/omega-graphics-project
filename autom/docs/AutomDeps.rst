==========
autom-deps
==========

``autom-deps`` is the AUTOM project dependency manager. It automates third-party
dependency fetching and project bootstrap for an Omega Graphics checkout based
on declarative ``automdeps.json`` manifests.

It is shipped as a thin shell/batch wrapper (``autom/tools/autom-deps`` and
``autom-deps.bat``) over the ``autom_deps`` Python package
(``autom/tools/autom_deps/``). The wrapper just forwards to
``autom_deps/main.py``.

==========

--------
Overview
--------

A run of ``autom-deps`` walks the manifest tree, evaluates per-item conditions,
and either fetches, refreshes, verifies, or cleans each dependency. State is
persisted between runs so that subsequent invocations can skip work that is
already satisfied.

Supported dependency kinds:

- **git** -- ``git clone`` / ``git fetch`` / ``git checkout`` of a repository
  to a destination, optionally pinned to a ``ref``.
- **file** -- single-file download to a destination, with optional ``size`` and
  ``sha256`` integrity checks.
- **archive** -- download + extract (zip or tar.\*) with ``strip_components``,
  ``dest_subdir``, ``expected_root`` and ``rename_root_to`` controls.
- **local** -- in-tree path that is registered (and exported) but not fetched.
- **tool** -- a wrapper around one of the above source kinds, recorded in
  state with ``type: tool`` so consumers can distinguish executables from
  source dependencies.

Each dependency may declare ``exports``. Exports are name-keyed paths
(``<dependency>.<export>``) that are resolved against the manifest directory
and surfaced to downstream consumers via the state file and the optional
CMake driver output.

==========

--------
Features
--------

**Manifest discovery and validation**
    Manifests are validated (``validation.py``) before execution. Duplicate
    dependency names and duplicate export names across manifests are rejected.

**Variables and string interpolation**
    Manifests may define ``variables`` blocks. References use the
    ``$(NAME)`` syntax and are resolved in fields such as ``url``, ``ref``,
    ``dest``, ``sha256``, ``archive_name``, ``dest_subdir``, ``expected_root``,
    ``rename_root_to``, and inside ``exports``. Cyclic references are detected
    and reported. See ``variables.py``.

**Conditional inclusion**
    Each dependency or command may be gated with ``platforms`` and/or a
    ``when`` block. The ``when`` block currently supports:

    - ``platform`` -- list of platforms (``windows``, ``macos``, ``linux``,
      ``ios``, ``android``).
    - ``arch`` -- list of normalized architectures (``x86_64``, ``arm64``,
      etc.; ``amd64``/``x64`` and ``aarch64`` are aliased).
    - ``exists`` -- list of paths (relative to the manifest directory) that
      must all exist for the item to be enabled.
    - ``not_exists`` -- list of paths that must all be absent.

    Skipped items are recorded in state with ``status: skipped`` and emit a
    ``SKIP ... (condition)`` line in the UI. See ``conditions.py``.

**Version sources**
    Dependencies may declare a ``version_source`` to resolve a moving target
    (e.g. a tag or release) into a concrete URL/ref. Resolved values are
    cached in state and re-used unless ``--resolve-stable`` or a refresh of
    the dependency is requested. See ``resolution.py``.

**Integrity verification**
    ``file`` and ``archive`` dependencies support ``size`` and ``sha256``.
    Mismatches raise ``DependencyExecutionError`` and abort the run.

**Incremental, resumable execution**
    Inputs (``url``, ``dest``, ``ref``, ``sha256``, ``size``,
    ``archive_name``, ``strip_components``, ``dest_subdir``,
    ``expected_root``, ``rename_root_to``) are recorded in state. Re-runs
    skip dependencies whose inputs match and whose destination is present.
    Downloads are streamed via ``DownloadManager`` and resumable unless
    ``--no-resume`` is set.

**State and exports**
    A state file is written under ``.automdeps/`` after every run. Resolved
    export paths are aggregated and can be emitted as a CMake include file
    via ``--cmake <path>`` (see ``exports.py``) or printed as JSON via
    ``--print-resolved <path|->``.

**Status UI**
    ``StatusPrinter`` prints labelled steps (``FETCH``, ``REFRESH``,
    ``SKIP``, ``VERIFY``, ``CLEAN``, ``DRYRUN``, ``INFO``) and a download
    progress bar. The UI auto-detects TTY; ``--progress`` /
    ``--no-progress`` / ``--quiet`` / ``--json-log`` /
    ``--single-stream`` adjust output for CI or non-interactive runs.

==========

-------------
Driver Usage
-------------

Invocation form::

    autom-deps [mode flags] [target/output flags] [ui flags]

The wrapper executes ``python3 autom_deps/main.py "$@"``, so any working
Python 3 on ``PATH`` is sufficient. There are no positional arguments;
manifest discovery starts from the current working directory.

^^^^^^^^^^^^^^^^^^^^^
Execution mode flags
^^^^^^^^^^^^^^^^^^^^^

Exactly one execution mode is in effect per run. They are mutually
constrained by ``cli.py``.

``--exec``
    Default. Fetch and materialize anything not yet satisfied.

``--sync``
    Re-run downloads/clones for every dependency to bring them up to date.
    Cannot be combined with ``--verify``, ``--dry-run``, ``--refresh``,
    ``--refresh-all`` or ``--clean``.

``--verify``
    Verify that every enabled dependency is materialized and (where
    applicable) that ``size`` / ``sha256`` match. Cannot be combined with
    ``--clean``, ``--refresh`` or ``--refresh-all``.

``--dry-run``
    Print what would happen without performing downloads, extractions,
    or destructive cleanup.

``--refresh NAME``
    Force re-fetch of the named dependency. May be repeated.

``--refresh-all``
    Force re-fetch of every dependency.

``--clean NAME``
    Remove materialized outputs of the named dependency. May be repeated.
    Not supported for ``local`` dependencies. Cannot be combined with
    ``--print-resolved``.

``--resolve-stable``
    Re-resolve every ``version_source`` even when a cached value is
    available in state.

^^^^^^^^^^^^^^^^^^^^
Target and output
^^^^^^^^^^^^^^^^^^^^

``--target {windows,macos,linux,ios,android}``
    Override the default platform used for ``platforms`` /
    ``when.platform`` evaluation. Defaults to the host OS.

``--print-resolved <path|->``
    Emit the resolved exports as JSON to a file (or stdout with ``-``).
    Cannot be combined with ``--cmake``.

``--cmake <path>``
    Emit a CMake include file with all resolved exports. Cannot be
    combined with ``--print-resolved``.

^^^^^^^^^^^
UI flags
^^^^^^^^^^^

``--verbose`` / ``--quiet``
    Mutually exclusive verbosity controls.

``--json-log``
    Emit machine-parseable JSON log lines instead of the human UI.

``--progress`` / ``--no-progress``
    Force-on or force-off the download progress bar. Default: on when
    stdout is a TTY and neither ``--quiet`` nor ``--json-log`` is set.

``--no-resume``
    Do not resume partial downloads from cache; always start from zero.

``--single-stream``
    Render UI as a single line stream (suitable for CI logs).

^^^^^^^^^^^^^^^^^
Exit behavior
^^^^^^^^^^^^^^^^^

- ``0`` -- success.
- ``1`` -- ``AutomDepsError`` (manifest validation, dependency execution,
  download or verification failure). The error is printed to stderr.
- ``130`` -- interrupted (Ctrl-C).

^^^^^^^^^^^^^^
Examples
^^^^^^^^^^^^^^

Bootstrap a fresh checkout::

    autom-deps

Re-resolve moving version sources and refresh a single dependency::

    autom-deps --resolve-stable --refresh wtk-core

Verify an existing checkout in CI::

    autom-deps --verify --json-log

Generate a CMake exports file for downstream ``find_package``-style use::

    autom-deps --cmake build/automdeps_exports.cmake

Cross-target evaluation (force Windows-only manifest gating from Linux)::

    autom-deps --target windows --dry-run

==========

----------------------------------
Argument-Conditional Fetching
----------------------------------

**Currently supported:** No.

``autom-deps`` does not currently accept user-supplied conditional flags
of the form ``--args GET_WTK_COMPONENTS=1`` (or similar) to gate which
dependencies are fetched. The CLI defined in ``autom_deps/cli.py`` does
not expose ``--arg``, ``--args``, or ``-D NAME=VALUE``, and the manifest
``variables`` block is populated only from the manifest itself -- there
is no override path from the command line into the variable scope.

The only conditional gating available today (``conditions.py``) is the
``when`` block on a per-dependency basis, keyed on:

- ``platform`` (overridable via ``--target``)
- ``arch`` (host-derived, normalized)
- ``exists`` / ``not_exists`` (filesystem probes against the manifest
  directory)

If you want a manifest like::

    {
      "name": "wtk-components",
      "type": "archive",
      "when": { "args": ["GET_WTK_COMPONENTS"] },
      ...
    }

driven by ``autom-deps --args GET_WTK_COMPONENTS ...``, that would
require:

1. A new repeatable CLI option (``--arg NAME[=VALUE]``, plausibly with
   ``-D`` as the short alias used elsewhere in AUTOM) parsed in
   ``cli.py``.
2. Flowing the parsed arg map through ``RunContext`` in ``runner.py``.
3. A new ``when.args`` (and/or ``when.not_args``) clause evaluated in
   ``conditions.py`` alongside ``platform`` / ``arch`` / ``exists``.
4. Optionally promoting parsed args into the manifest variable scope so
   they can be referenced as ``$(GET_WTK_COMPONENTS)`` inside
   ``url`` / ``ref`` / ``dest`` interpolation.

None of these hooks exist in the current code.
