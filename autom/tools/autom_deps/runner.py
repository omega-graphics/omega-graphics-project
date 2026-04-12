from __future__ import annotations

import json
import os
import platform
import runpy
import shutil
import tarfile
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

import requests

from .conditions import item_enabled
from .dependencies import execute_dependency
from .errors import CommandExecutionError, ManifestValidationError
from .state import exports_from_state, load_state, save_exports, save_state
from .validation import validate_manifest
from .variables import resolve_string, validate_local_variables


@dataclass
class StatusPrinter:
    total_steps: int
    current_step: int = 0

    def header(self, root_dir: str, target: str, mode: str) -> None:
        print(f"[INFO] root={root_dir}")
        print(f"[INFO] target={target}")
        print(f"[INFO] mode={mode}")

    def step(self, kind: str, message: str) -> None:
        self.current_step += 1
        if self.current_step > self.total_steps:
            self.total_steps = self.current_step
        print(f"[{self.current_step}/{self.total_steps}] {kind:<8} {message}")

    def note(self, kind: str, message: str) -> None:
        print(f"[{kind}] {message}")


@dataclass
class RunContext:
    default_platform: str
    absroot: str
    update_only: bool
    verify_only: bool
    dry_run: bool
    refresh_all: bool
    refresh_targets: set[str]
    clean_targets: set[str]
    resolve_stable: bool
    print_resolved: str | None
    variables: dict[str, object] = field(default_factory=dict)
    clone_automdeps_queue: list[str] = field(default_factory=list)
    state: dict = field(default_factory=lambda: {"dependencies": {}})
    exports: dict[str, str] = field(default_factory=dict)
    seen_dependency_names: set[str] = field(default_factory=set)
    seen_export_names: set[str] = field(default_factory=set)
    printer: StatusPrinter | None = None


def detect_default_platform(target: str | None) -> str:
    if target:
        return target
    system_name = platform.system()
    if system_name == "Windows":
        return "windows"
    if system_name == "Darwin":
        return "macos"
    return "linux"


def execution_mode_label(args) -> str:
    if args.update:
        return "sync"
    if args.verify:
        return "verify"
    if args.clean:
        return "clean"
    if args.dry_run:
        return "dry-run"
    if args.refresh_all:
        return "refresh-all"
    if args.refresh:
        return "refresh"
    return "exec"


def load_manifest(manifest_path: Path) -> dict:
    try:
        with manifest_path.open("r", encoding="utf-8") as stream:
            manifest = json.load(stream)
    except FileNotFoundError as ex:
        raise ManifestValidationError("AUTOMDEPS file not found", manifest_path=str(manifest_path)) from ex
    except json.JSONDecodeError as ex:
        raise ManifestValidationError(
            f"invalid JSON: {ex.msg}",
            manifest_path=str(manifest_path),
            field=f"line {ex.lineno}, column {ex.colno}",
        ) from ex

    validate_manifest(manifest, str(manifest_path))
    return manifest


def count_commands_recursive(manifest_path: Path, include_root_commands: bool = True) -> int:
    manifest = load_manifest(manifest_path)
    total = 0

    if include_root_commands:
        total += len(manifest.get("rootCommands", []))
        total += len(manifest.get("postRootCommands", []))
    total += len(manifest.get("dependencies", []))
    total += len(manifest.get("commands", []))
    total += len(manifest.get("postCommands", []))

    manifest_dir = manifest_path.parent
    for subdir in manifest.get("subdirs", []):
        total += count_commands_recursive((manifest_dir / subdir / "AUTOMDEPS").resolve(), include_root_commands=False)
    return total


def _resolve_command_string(ctx: RunContext, value: str, manifest_path: Path, field: str) -> str:
    return resolve_string(value, ctx.variables, str(manifest_path), field)


def _run_shell(command: str, cwd: str, *, manifest_path: Path, command_type: str, command_index: int) -> None:
    prior_dir = os.getcwd()
    try:
        os.chdir(cwd)
        exit_code = os.system(command)
    finally:
        os.chdir(prior_dir)
    if exit_code != 0:
        raise CommandExecutionError(
            f"shell command failed with exit code {exit_code}",
            manifest_path=str(manifest_path),
            command_type=command_type,
            command_index=command_index,
        )


def _command_detail(command_type: str, resolved: dict[str, str]) -> str:
    for key in ("url", "path", "zipfile", "tarfile", "dir", "dest"):
        if key in resolved:
            return f"{command_type} {resolved[key]}"
    return command_type


def _is_existing_git_checkout(path: str) -> bool:
    git_path = Path(path) / ".git"
    return os.path.isdir(path) and git_path.exists()


def process_command(
    ctx: RunContext,
    command: dict,
    manifest_path: Path,
    command_index: int,
    command_field: str,
) -> None:
    command_type = command["type"]
    if not item_enabled(command, ctx.default_platform, ctx.variables, str(manifest_path), command_field, manifest_path.parent):
        ctx.printer.step("SKIP", f"{command_type} (condition)")  # type: ignore[union-attr]
        return

    if ctx.clean_targets:
        ctx.printer.step("SKIP", f"{command_type} (clean mode)")  # type: ignore[union-attr]
        return

    if ctx.verify_only:
        ctx.printer.step("SKIP", f"{command_type} (verify mode)")  # type: ignore[union-attr]
        return

    if ctx.update_only and command_type not in {"git_clone", "clone"}:
        ctx.printer.step("SKIP", f"{command_type} (sync mode)")  # type: ignore[union-attr]
        return

    resolved: dict[str, str] = {}
    for key, value in command.items():
        if key in {"type", "platforms", "when"}:
            continue
        if isinstance(value, str):
            resolved[key] = _resolve_command_string(ctx, value, manifest_path, f"{command_field}[{command_index}].{key}")

    step_kind = "DRYRUN" if ctx.dry_run else "RUN"
    ctx.printer.step(step_kind, _command_detail(command_type, resolved))  # type: ignore[union-attr]

    if ctx.dry_run:
        return

    if command_type in {"git_clone", "clone"}:
        url = resolved["url"]
        dest = resolved["dest"]
        branch = None if resolved["branch"] == "default" else resolved["branch"]
        if ctx.update_only:
            if not os.path.isdir(dest):
                raise CommandExecutionError(
                    f"cannot sync missing repository '{dest}'",
                    manifest_path=str(manifest_path),
                    command_type=command_type,
                    command_index=command_index,
                )
            _run_shell("git pull", dest, manifest_path=manifest_path, command_type=command_type, command_index=command_index)
        else:
            if os.path.exists(dest):
                if not _is_existing_git_checkout(dest):
                    raise CommandExecutionError(
                        f"destination exists and is not a git checkout: '{dest}'",
                        manifest_path=str(manifest_path),
                        command_type=command_type,
                        command_index=command_index,
                    )
                ctx.printer.note("OK", f"using existing repository {os.path.abspath(dest)}")  # type: ignore[union-attr]
            else:
                clone_cmd = f"git clone {url} {dest}" if branch is None else f"git clone {url} --branch {branch} {dest}"
                _run_shell(clone_cmd, os.getcwd(), manifest_path=manifest_path, command_type=command_type, command_index=command_index)
            if command_type == "clone":
                ctx.clone_automdeps_queue.append(os.path.abspath(dest))
        return

    if command_type == "chdir":
        os.chdir(resolved["dir"])
        return

    if command_type == "system":
        _run_shell(resolved["path"], os.getcwd(), manifest_path=manifest_path, command_type=command_type, command_index=command_index)
        return

    if command_type == "script":
        script_path = Path(resolved["path"]).resolve()
        args = [
            resolve_string(arg, ctx.variables, str(manifest_path), f"{command_field}[{command_index}].args[{index}]")
            for index, arg in enumerate(command["args"])
        ]
        old_argv = list(os.sys.argv)
        prior_dir = os.getcwd()
        try:
            os.sys.argv = [str(script_path)] + args
            os.chdir(str(script_path.parent))
            runpy.run_path(str(script_path), run_name="__main__")
        finally:
            os.sys.argv = old_argv
            os.chdir(prior_dir)
        return

    if command_type == "download":
        url = resolved["url"]
        dest = resolved["dest"]
        dest_dir = os.path.dirname(dest)
        if dest_dir:
            os.makedirs(dest_dir, exist_ok=True)
        temp_dest = dest + ".part"
        try:
            with requests.get(
                url,
                stream=True,
                allow_redirects=True,
                timeout=(30, 300),
                headers={"User-Agent": "autom-deps/1.0"},
            ) as response:
                response.raise_for_status()
                total_bytes = 0
                with open(temp_dest, "wb") as out:
                    for chunk in response.iter_content(chunk_size=1024 * 1024):
                        if not chunk:
                            continue
                        out.write(chunk)
                        total_bytes += len(chunk)
                os.replace(temp_dest, dest)
                ctx.printer.note("OK", f"downloaded {total_bytes} bytes from {response.url}")  # type: ignore[union-attr]
        except requests.RequestException as ex:
            if os.path.exists(temp_dest):
                os.remove(temp_dest)
            raise CommandExecutionError(
                f"download failed: {ex}",
                manifest_path=str(manifest_path),
                command_type=command_type,
                command_index=command_index,
            ) from ex
        return

    if command_type == "tar":
        archive_path = resolved["tarfile"]
        dest = resolved["dest"]
        with tarfile.open(archive_path, "r:*") as tar:
            tar.extractall(dest)
        os.remove(archive_path)
        shutil.rmtree(os.path.dirname(archive_path), ignore_errors=True)
        return

    if command_type == "unzip":
        zip_path = resolved["zipfile"]
        dest = resolved["dest"]
        with zipfile.ZipFile(zip_path, "r") as archive:
            archive.extractall(dest)
        os.remove(zip_path)
        shutil.rmtree(os.path.dirname(zip_path), ignore_errors=True)
        return

    raise CommandExecutionError(
        f"unhandled command type '{command_type}'",
        manifest_path=str(manifest_path),
        command_type=command_type,
        command_index=command_index,
    )


def _replace_dependency_exports(ctx: RunContext, dependency_name: str, exports: dict[str, str]) -> None:
    prefix = f"{dependency_name}."
    stale_keys = [key for key in ctx.exports if key.startswith(prefix)]
    for key in stale_keys:
        del ctx.exports[key]
    ctx.exports.update(exports)


def run_manifest(ctx: RunContext, manifest_path: Path, is_root_manifest: bool) -> None:
    manifest = load_manifest(manifest_path)
    manifest_dir = manifest_path.parent

    local_variables = manifest.get("variables", {})
    validate_local_variables(local_variables, ctx.variables, str(manifest_path))

    prior_dir = os.getcwd()
    prior_variables = dict(ctx.variables)
    try:
        os.chdir(str(manifest_dir))
        ctx.variables.update(local_variables)

        if is_root_manifest:
            for index, command in enumerate(manifest.get("rootCommands", [])):
                process_command(ctx, command, manifest_path, index, "rootCommands")

        for index, dependency in enumerate(manifest.get("dependencies", [])):
            result = execute_dependency(ctx, dependency, manifest_path, field=f"dependencies[{index}]")
            _replace_dependency_exports(ctx, result.name, result.exports)
            if result.state_entry is None:
                ctx.state["dependencies"].pop(result.name, None)
            else:
                ctx.state["dependencies"][result.name] = result.state_entry

        for index, command in enumerate(manifest.get("commands", [])):
            process_command(ctx, command, manifest_path, index, "commands")

        for subdir in manifest.get("subdirs", []):
            child_manifest_path = (manifest_dir / subdir / "AUTOMDEPS").resolve()
            ctx.printer.note("INFO", f"entering subdir {os.path.relpath(child_manifest_path.parent, start=ctx.absroot)}")  # type: ignore[union-attr]
            run_manifest(ctx, child_manifest_path, is_root_manifest=False)

        for index, command in enumerate(manifest.get("postCommands", [])):
            process_command(ctx, command, manifest_path, index, "postCommands")

        if is_root_manifest:
            for index, command in enumerate(manifest.get("postRootCommands", [])):
                process_command(ctx, command, manifest_path, index, "postRootCommands")
    finally:
        ctx.variables.clear()
        ctx.variables.update(prior_variables)
        os.chdir(prior_dir)


def _print_resolved(state: dict, query: str) -> int:
    if "." not in query:
        raise ManifestValidationError(
            "print-resolved expects '<dependency>.<field>'",
            field="print_resolved",
        )
    dependency_name, field_name = query.split(".", 1)
    entry = state.get("dependencies", {}).get(dependency_name)
    if not isinstance(entry, dict):
        raise ManifestValidationError(
            f"no resolved state found for dependency '{dependency_name}'",
            field="print_resolved",
        )
    resolved = entry.get("resolved", {})
    if not isinstance(resolved, dict) or field_name not in resolved:
        raise ManifestValidationError(
            f"resolved field '{field_name}' was not found for dependency '{dependency_name}'",
            field="print_resolved",
        )
    print(resolved[field_name])
    return 0


def run(argv: list[str] | None, args) -> int:
    del argv

    manifest_path = Path.cwd() / "AUTOMDEPS"
    if not manifest_path.exists():
        raise ManifestValidationError("AUTOMDEPS file not found in current directory", manifest_path=str(manifest_path))

    state = load_state(Path.cwd())
    if args.print_resolved:
        return _print_resolved(state, args.print_resolved)

    default_platform = detect_default_platform(args.target)
    total_steps = count_commands_recursive(manifest_path)
    ctx = RunContext(
        default_platform=default_platform,
        absroot=str(Path.cwd()),
        update_only=args.update,
        verify_only=args.verify,
        dry_run=args.dry_run,
        refresh_all=args.refresh_all,
        refresh_targets=set(args.refresh),
        clean_targets=set(args.clean),
        resolve_stable=args.resolve_stable,
        print_resolved=args.print_resolved,
        state=state,
        exports=exports_from_state(state) if args.clean else {},
        printer=StatusPrinter(total_steps=max(total_steps, 1)),
    )

    ctx.printer.header(ctx.absroot, ctx.default_platform, execution_mode_label(args))
    run_manifest(ctx, manifest_path.resolve(), is_root_manifest=True)

    if ctx.clone_automdeps_queue and not (ctx.dry_run or ctx.verify_only or ctx.clean_targets):
        for clone_root in list(ctx.clone_automdeps_queue):
            clone_manifest = Path(clone_root) / "AUTOMDEPS"
            if not clone_manifest.exists():
                continue
            ctx.printer.note("INFO", f"entering cloned component {os.path.relpath(clone_root, start=ctx.absroot)}")
            run_manifest(ctx, clone_manifest.resolve(), is_root_manifest=True)

    if not ctx.dry_run:
        save_state(ctx.absroot, ctx.state)
        save_exports(ctx.absroot, ctx.exports)
    ctx.printer.note("DONE", "autom-deps completed successfully")
    return 0
