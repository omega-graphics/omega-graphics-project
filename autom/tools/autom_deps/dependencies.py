from __future__ import annotations

import hashlib
import os
import shutil
import tarfile
import urllib.parse
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import requests

from .conditions import item_enabled
from .errors import DependencyExecutionError, ManifestValidationError
from .resolution import apply_resolved_version_source, cleaned_paths_from_state, resolve_version_source
from .variables import resolve_string

if TYPE_CHECKING:
    from .runner import RunContext


@dataclass
class DependencyResult:
    name: str
    exports: dict[str, str]
    state_entry: dict | None
    skipped: bool = False


def _run_shell(command: str, cwd: str, *, manifest_path: Path, dependency_name: str, dependency_type: str) -> None:
    prior_dir = os.getcwd()
    try:
        os.chdir(cwd)
        exit_code = os.system(command)
    finally:
        os.chdir(prior_dir)
    if exit_code != 0:
        raise DependencyExecutionError(
            f"shell command failed with exit code {exit_code}",
            manifest_path=str(manifest_path),
            dependency_name=dependency_name,
            dependency_type=dependency_type,
        )


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _verify_expected_size(path: Path, expected_size: int | None, *, manifest_path: Path, dependency_name: str, dependency_type: str) -> None:
    if expected_size is None:
        return
    size = path.stat().st_size
    if size != expected_size:
        raise DependencyExecutionError(
            f"size mismatch for '{path}': expected {expected_size}, got {size}",
            manifest_path=str(manifest_path),
            dependency_name=dependency_name,
            dependency_type=dependency_type,
        )


def _verify_expected_sha256(path: Path, expected_sha256: str | None, *, manifest_path: Path, dependency_name: str, dependency_type: str) -> str | None:
    if expected_sha256 is None:
        return None
    actual_sha256 = _sha256_file(path)
    if actual_sha256.lower() != expected_sha256.lower():
        raise DependencyExecutionError(
            f"sha256 mismatch for '{path}'",
            manifest_path=str(manifest_path),
            dependency_name=dependency_name,
            dependency_type=dependency_type,
        )
    return actual_sha256


def _verify_file_artifact(
    path: Path,
    *,
    expected_size: int | None,
    expected_sha256: str | None,
    manifest_path: Path,
    dependency_name: str,
    dependency_type: str,
) -> dict[str, object]:
    if not path.exists():
        raise DependencyExecutionError(
            f"required artifact is missing: {path}",
            manifest_path=str(manifest_path),
            dependency_name=dependency_name,
            dependency_type=dependency_type,
        )
    if not path.is_file():
        raise DependencyExecutionError(
            f"required artifact is not a file: {path}",
            manifest_path=str(manifest_path),
            dependency_name=dependency_name,
            dependency_type=dependency_type,
        )

    _verify_expected_size(path, expected_size, manifest_path=manifest_path, dependency_name=dependency_name, dependency_type=dependency_type)
    actual_sha256 = _verify_expected_sha256(
        path,
        expected_sha256,
        manifest_path=manifest_path,
        dependency_name=dependency_name,
        dependency_type=dependency_type,
    )
    verification = {"exists": True, "size": path.stat().st_size}
    if actual_sha256:
        verification["sha256"] = actual_sha256
    return verification


def _resolve_exports(ctx: "RunContext", dependency: dict, manifest_path: Path, field: str) -> dict[str, str]:
    exports = dependency.get("exports", {})
    resolved = {}
    for key, value in exports.items():
        export_value = resolve_string(value, ctx.variables, str(manifest_path), f"{field}.exports.{key}")
        path = Path(export_value)
        if not path.is_absolute():
            path = (manifest_path.parent / path).resolve()
        resolved[f"{dependency['name']}.{key}"] = str(path)
    return resolved


def _default_root_export(dependency: dict, manifest_path: Path) -> dict[str, str]:
    if "dest" in dependency:
        root = Path(dependency["dest"])
        if not root.is_absolute():
            root = (manifest_path.parent / root).resolve()
        return {f"{dependency['name']}.root": str(root)}
    if dependency["type"] == "local":
        root = Path(dependency["path"])
        if not root.is_absolute():
            root = (manifest_path.parent / root).resolve()
        return {f"{dependency['name']}.root": str(root)}
    return {}


def _record_state_entry(
    dependency_name: str,
    dependency_type: str,
    manifest_path: Path,
    status: str,
    *,
    inputs: dict[str, object] | None = None,
    resolved: dict[str, object] | None = None,
    outputs: dict[str, object] | None = None,
    verification: dict[str, object] | None = None,
    exports: dict[str, str] | None = None,
) -> dict:
    entry = {
        "name": dependency_name,
        "type": dependency_type,
        "manifest": str(manifest_path),
        "status": status,
    }
    if inputs:
        entry["inputs"] = inputs
    if resolved:
        entry["resolved"] = resolved
    if outputs:
        entry["outputs"] = outputs
    if verification:
        entry["verification"] = verification
    if exports:
        entry["exports"] = exports
    return entry


def _entry_exports(entry: dict | None) -> dict[str, str]:
    if not isinstance(entry, dict):
        return {}
    exports = entry.get("exports", {})
    if not isinstance(exports, dict):
        return {}
    return {
        key: value
        for key, value in exports.items()
        if isinstance(key, str) and isinstance(value, str)
    }


def _preserve_existing_result(name: str, entry: dict | None, reason: str, ctx: "RunContext") -> DependencyResult:
    ctx.printer.step("SKIP", f"{name} ({reason})")  # type: ignore[union-attr]
    return DependencyResult(name=name, exports=_entry_exports(entry), state_entry=entry, skipped=True)


def _dependency_skipped(ctx: "RunContext", dependency: dict, manifest_path: Path) -> DependencyResult:
    dep_type = dependency["type"]
    ctx.printer.step("SKIP", f"{dep_type} {dependency['name']} (condition)")  # type: ignore[union-attr]
    return DependencyResult(
        name=dependency["name"],
        exports={},
        state_entry=_record_state_entry(dependency["name"], dep_type, manifest_path, "skipped"),
        skipped=True,
    )


def _guess_archive_name(url: str, fallback_name: str) -> str:
    parsed = urllib.parse.urlparse(url)
    name = os.path.basename(parsed.path)
    return name or fallback_name


def _copy_tree_contents(source: Path, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    for entry in source.iterdir():
        target = dest / entry.name
        if entry.is_dir():
            shutil.copytree(entry, target, dirs_exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(entry, target)


def _extract_archive_with_strip(archive_path: Path, extract_dest: Path, strip_components: int) -> None:
    extract_dest.mkdir(parents=True, exist_ok=True)
    if zipfile.is_zipfile(archive_path):
        with zipfile.ZipFile(archive_path, "r") as archive:
            for member in archive.infolist():
                member_path = Path(member.filename)
                parts = member_path.parts[strip_components:]
                if not parts:
                    continue
                target = extract_dest.joinpath(*parts)
                if member.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(member, "r") as src, target.open("wb") as dst:
                    shutil.copyfileobj(src, dst)
        return

    with tarfile.open(archive_path, "r:*") as archive:
        for member in archive.getmembers():
            member_path = Path(member.name)
            parts = member_path.parts[strip_components:]
            if not parts:
                continue
            target = extract_dest.joinpath(*parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            extracted = archive.extractfile(member)
            if extracted is None:
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with extracted, target.open("wb") as dst:
                shutil.copyfileobj(extracted, dst)


def _download_to_file(
    url: str,
    dest: Path,
    *,
    manifest_path: Path,
    dependency_name: str,
    dependency_type: str,
    printer,
    dry_run: bool,
) -> str:
    if dry_run:
        printer.note("DRYRUN", f"would download {url} -> {dest}")
        return url

    dest.parent.mkdir(parents=True, exist_ok=True)
    temp_dest = Path(str(dest) + ".part")
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
            with temp_dest.open("wb") as out:
                for chunk in response.iter_content(chunk_size=1024 * 1024):
                    if not chunk:
                        continue
                    out.write(chunk)
                    total_bytes += len(chunk)
            os.replace(temp_dest, dest)
            printer.note("OK", f"downloaded {total_bytes} bytes from {response.url}")
            return str(response.url)
    except requests.RequestException as ex:
        if temp_dest.exists():
            temp_dest.unlink()
        raise DependencyExecutionError(
            f"download failed: {ex}",
            manifest_path=str(manifest_path),
            dependency_name=dependency_name,
            dependency_type=dependency_type,
        ) from ex


def _should_refresh(ctx: "RunContext", dependency_name: str) -> bool:
    if ctx.refresh_all:
        return True
    if dependency_name in ctx.refresh_targets:
        return True
    return False


def _should_clean(ctx: "RunContext", dependency_name: str) -> bool:
    return dependency_name in ctx.clean_targets


def _resolve_version_metadata(ctx: "RunContext", dependency: dict, current_entry: dict | None) -> tuple[dict, dict[str, str]]:
    version_source = dependency.get("version_source")
    if not isinstance(version_source, dict):
        return dependency, {}

    cached_resolved = {}
    if isinstance(current_entry, dict):
        cached = current_entry.get("resolved", {})
        if isinstance(cached, dict):
            cached_resolved = {
                key: value
                for key, value in cached.items()
                if isinstance(key, str) and isinstance(value, str)
            }

    needs_fresh_resolution = ctx.resolve_stable or _should_refresh(ctx, dependency["name"])
    if not needs_fresh_resolution and cached_resolved:
        return apply_resolved_version_source(dependency, cached_resolved), cached_resolved

    resolved = resolve_version_source(version_source, dependency)
    ctx.printer.note("INFO", f"resolved {dependency['name']} -> {resolved}")  # type: ignore[union-attr]
    return apply_resolved_version_source(dependency, resolved), resolved


def _clean_dependency_outputs(ctx: "RunContext", dependency: dict, current_entry: dict | None, manifest_path: Path) -> DependencyResult:
    dep_type = dependency["type"]
    name = dependency["name"]
    if dep_type == "local":
        raise DependencyExecutionError(
            "clean is not supported for local dependencies",
            manifest_path=str(manifest_path),
            dependency_name=name,
            dependency_type=dep_type,
        )

    ctx.printer.step("CLEAN", f"{dep_type} {name}")  # type: ignore[union-attr]
    if ctx.dry_run:
        for path in cleaned_paths_from_state(current_entry or {}):
            ctx.printer.note("DRYRUN", f"would remove {path}")  # type: ignore[union-attr]
        return DependencyResult(name=name, exports={}, state_entry=None, skipped=False)

    for path in cleaned_paths_from_state(current_entry or {}):
        if path.is_dir():
            shutil.rmtree(path, ignore_errors=True)
        elif path.exists():
            path.unlink()
    return DependencyResult(name=name, exports={}, state_entry=None, skipped=False)


def _dependency_inputs_match(current_entry: dict | None, desired_inputs: dict[str, object]) -> bool:
    if not isinstance(current_entry, dict):
        return False
    current_inputs = current_entry.get("inputs")
    if not isinstance(current_inputs, dict):
        return False
    return current_inputs == desired_inputs


def _execute_git(ctx: "RunContext", dependency: dict, manifest_path: Path, current_entry: dict | None) -> DependencyResult:
    name = dependency["name"]
    dest = Path(resolve_string(dependency["dest"], ctx.variables, str(manifest_path), f"dependencies.{name}.dest"))
    if not dest.is_absolute():
        dest = (manifest_path.parent / dest).resolve()
    had_dest = dest.is_dir()
    url = resolve_string(dependency["url"], ctx.variables, str(manifest_path), f"dependencies.{name}.url")
    ref = dependency.get("ref")
    if isinstance(ref, str):
        ref = resolve_string(ref, ctx.variables, str(manifest_path), f"dependencies.{name}.ref")

    resolved_dependency, resolved_version = _resolve_version_metadata(
        ctx,
        {**dependency, "url": url, "ref": ref, "dest": str(dest)},
        current_entry,
    )
    url = str(resolved_dependency["url"])
    ref = resolved_dependency.get("ref")
    if not isinstance(ref, str):
        ref = None

    desired_inputs = {"url": url, "dest": str(dest)}
    if ref:
        desired_inputs["ref"] = ref

    exports = _default_root_export({"name": name, "type": "git", "dest": str(dest)}, manifest_path)
    exports.update(_resolve_exports(ctx, {**dependency, "dest": str(dest)}, manifest_path, f"dependencies.{name}"))
    outputs = {"dest": str(dest)}

    if ctx.verify_only:
        ctx.printer.step("VERIFY", f"git {name} -> {dest}")  # type: ignore[union-attr]
        if not dest.is_dir():
            raise DependencyExecutionError(
                f"repository is missing: {dest}",
                manifest_path=str(manifest_path),
                dependency_name=name,
                dependency_type="git",
            )
        return DependencyResult(
            name=name,
            exports=exports,
            state_entry=_record_state_entry(
                name,
                "git",
                manifest_path,
                "verified",
                inputs=desired_inputs,
                resolved=resolved_version,
                outputs=outputs,
                verification={"exists": True},
                exports=exports,
            ),
        )

    if not ctx.update_only and not _should_refresh(ctx, name) and had_dest and (
        _dependency_inputs_match(current_entry, desired_inputs) or current_entry is None
    ):
        ctx.printer.step("SKIP", f"git {name} -> {dest}")  # type: ignore[union-attr]
        return DependencyResult(
            name=name,
            exports=exports,
            state_entry=_record_state_entry(
                name,
                "git",
                manifest_path,
                "present",
                inputs=desired_inputs,
                resolved=resolved_version,
                outputs=outputs,
                verification={"exists": True},
                exports=exports,
            ),
            skipped=True,
        )

    phase = "REFRESH" if had_dest else "FETCH"
    ctx.printer.step(phase, f"git {name} -> {dest}")  # type: ignore[union-attr]
    if ctx.dry_run:
        clone_message = "would update repository" if had_dest else f"would clone {url} -> {dest}"
        ctx.printer.note("DRYRUN", clone_message)  # type: ignore[union-attr]
    else:
        if had_dest:
            _run_shell("git fetch --all --tags", str(dest), manifest_path=manifest_path, dependency_name=name, dependency_type="git")
        else:
            clone_cmd = f"git clone {url} {dest}"
            _run_shell(clone_cmd, str(manifest_path.parent), manifest_path=manifest_path, dependency_name=name, dependency_type="git")
        if ref:
            _run_shell(f"git checkout {ref}", str(dest), manifest_path=manifest_path, dependency_name=name, dependency_type="git")
        elif had_dest:
            _run_shell("git pull", str(dest), manifest_path=manifest_path, dependency_name=name, dependency_type="git")

    if ctx.dry_run:
        status = "planned"
    else:
        status = "refreshed" if had_dest else "cloned"
    return DependencyResult(
        name=name,
        exports=exports,
        state_entry=_record_state_entry(
            name,
            "git",
            manifest_path,
            status,
            inputs=desired_inputs,
            resolved=resolved_version,
            outputs=outputs,
            verification={"exists": True},
            exports=exports,
        ),
    )


def _execute_file(ctx: "RunContext", dependency: dict, manifest_path: Path, current_entry: dict | None) -> DependencyResult:
    name = dependency["name"]
    dest = Path(resolve_string(dependency["dest"], ctx.variables, str(manifest_path), f"dependencies.{name}.dest"))
    if not dest.is_absolute():
        dest = (manifest_path.parent / dest).resolve()
    url = resolve_string(dependency["url"], ctx.variables, str(manifest_path), f"dependencies.{name}.url")
    expected_sha256 = dependency.get("sha256")
    if isinstance(expected_sha256, str):
        expected_sha256 = resolve_string(expected_sha256, ctx.variables, str(manifest_path), f"dependencies.{name}.sha256")
    else:
        expected_sha256 = None
    expected_size = dependency.get("size")
    if expected_size is not None:
        expected_size = int(expected_size)

    resolved_dependency, resolved_version = _resolve_version_metadata(
        ctx,
        {**dependency, "url": url, "dest": str(dest)},
        current_entry,
    )
    url = str(resolved_dependency["url"])

    desired_inputs = {"url": url, "dest": str(dest)}
    if expected_sha256:
        desired_inputs["sha256"] = expected_sha256
    if expected_size is not None:
        desired_inputs["size"] = expected_size

    outputs = {"dest": str(dest)}
    exports = _default_root_export({"name": name, "type": "file", "dest": str(dest)}, manifest_path)
    exports.update(_resolve_exports(ctx, {**dependency, "dest": str(dest)}, manifest_path, f"dependencies.{name}"))

    if ctx.verify_only:
        ctx.printer.step("VERIFY", f"file {name} -> {dest}")  # type: ignore[union-attr]
        verification = _verify_file_artifact(
            dest,
            expected_size=expected_size,
            expected_sha256=expected_sha256,
            manifest_path=manifest_path,
            dependency_name=name,
            dependency_type="file",
        )
        return DependencyResult(
            name=name,
            exports=exports,
            state_entry=_record_state_entry(
                name,
                "file",
                manifest_path,
                "verified",
                inputs=desired_inputs,
                resolved=resolved_version,
                outputs=outputs,
                verification=verification,
                exports=exports,
            ),
        )

    satisfied = False
    verification: dict[str, object] | None = None
    if not ctx.update_only and not _should_refresh(ctx, name) and dest.exists():
        if _dependency_inputs_match(current_entry, desired_inputs) or current_entry is None:
            verification = _verify_file_artifact(
                dest,
                expected_size=expected_size,
                expected_sha256=expected_sha256,
                manifest_path=manifest_path,
                dependency_name=name,
                dependency_type="file",
            )
            satisfied = True

    if satisfied:
        ctx.printer.step("SKIP", f"file {name} -> {dest}")  # type: ignore[union-attr]
        return DependencyResult(
            name=name,
            exports=exports,
            state_entry=_record_state_entry(
                name,
                "file",
                manifest_path,
                "present",
                inputs=desired_inputs,
                resolved=resolved_version,
                outputs=outputs,
                verification=verification,
                exports=exports,
            ),
            skipped=True,
        )

    phase = "REFRESH" if (dest.exists() or _should_refresh(ctx, name) or ctx.update_only) else "FETCH"
    ctx.printer.step(phase, f"file {name} -> {dest}")  # type: ignore[union-attr]
    final_url = _download_to_file(
        url,
        dest,
        manifest_path=manifest_path,
        dependency_name=name,
        dependency_type="file",
        printer=ctx.printer,
        dry_run=ctx.dry_run,
    )

    if ctx.dry_run:
        verification = {}
    else:
        verification = _verify_file_artifact(
            dest,
            expected_size=expected_size,
            expected_sha256=expected_sha256,
            manifest_path=manifest_path,
            dependency_name=name,
            dependency_type="file",
        )

    resolved_state = dict(resolved_version)
    resolved_state["url"] = url
    resolved_state["final_url"] = final_url
    return DependencyResult(
        name=name,
        exports=exports,
        state_entry=_record_state_entry(
            name,
            "file",
            manifest_path,
            "downloaded" if not ctx.dry_run else "planned",
            inputs=desired_inputs,
            resolved=resolved_state,
            outputs=outputs,
            verification=verification,
            exports=exports,
        ),
    )


def _execute_archive(ctx: "RunContext", dependency: dict, manifest_path: Path, current_entry: dict | None) -> DependencyResult:
    name = dependency["name"]
    dest = Path(resolve_string(dependency["dest"], ctx.variables, str(manifest_path), f"dependencies.{name}.dest"))
    if not dest.is_absolute():
        dest = (manifest_path.parent / dest).resolve()
    url = resolve_string(dependency["url"], ctx.variables, str(manifest_path), f"dependencies.{name}.url")
    archive_name = dependency.get("archive_name")
    if isinstance(archive_name, str):
        archive_name = resolve_string(archive_name, ctx.variables, str(manifest_path), f"dependencies.{name}.archive_name")
    else:
        archive_name = _guess_archive_name(url, f"{name}.archive")

    expected_sha256 = dependency.get("sha256")
    if isinstance(expected_sha256, str):
        expected_sha256 = resolve_string(expected_sha256, ctx.variables, str(manifest_path), f"dependencies.{name}.sha256")
    else:
        expected_sha256 = None
    expected_size = dependency.get("size")
    if expected_size is not None:
        expected_size = int(expected_size)

    strip_components = int(dependency.get("strip_components", 0))
    dest_subdir = dependency.get("dest_subdir")
    expected_root = dependency.get("expected_root")
    rename_root_to = dependency.get("rename_root_to")
    if isinstance(dest_subdir, str):
        dest_subdir = resolve_string(dest_subdir, ctx.variables, str(manifest_path), f"dependencies.{name}.dest_subdir")
    if isinstance(expected_root, str):
        expected_root = resolve_string(expected_root, ctx.variables, str(manifest_path), f"dependencies.{name}.expected_root")
    if isinstance(rename_root_to, str):
        rename_root_to = resolve_string(rename_root_to, ctx.variables, str(manifest_path), f"dependencies.{name}.rename_root_to")

    resolved_dependency, resolved_version = _resolve_version_metadata(
        ctx,
        {**dependency, "url": url, "dest": str(dest), "archive_name": archive_name},
        current_entry,
    )
    url = str(resolved_dependency["url"])
    if "archive_name" in resolved_dependency and isinstance(resolved_dependency["archive_name"], str):
        archive_name = resolved_dependency["archive_name"]

    extract_dest = dest / dest_subdir if dest_subdir else dest
    cache_dir = Path(ctx.absroot) / ".automdeps" / "cache" / name
    temp_extract_dir = Path(ctx.absroot) / ".automdeps" / "tmp" / name
    archive_path = cache_dir / archive_name

    desired_inputs = {
        "url": url,
        "dest": str(extract_dest),
        "archive_name": archive_name,
        "strip_components": strip_components,
        "dest_subdir": dest_subdir,
        "expected_root": expected_root,
        "rename_root_to": rename_root_to,
    }
    if expected_sha256:
        desired_inputs["sha256"] = expected_sha256
    if expected_size is not None:
        desired_inputs["size"] = expected_size

    outputs = {
        "dest": str(extract_dest),
        "cache_dir": str(cache_dir),
        "temp_dir": str(temp_extract_dir),
        "archive_path": str(archive_path),
    }
    exports = _default_root_export({"name": name, "type": "archive", "dest": str(extract_dest)}, manifest_path)
    exports.update(_resolve_exports(ctx, {**dependency, "dest": str(extract_dest)}, manifest_path, f"dependencies.{name}"))

    if ctx.verify_only:
        ctx.printer.step("VERIFY", f"archive {name} -> {extract_dest}")  # type: ignore[union-attr]
        if not extract_dest.exists():
            raise DependencyExecutionError(
                f"extracted dependency is missing: {extract_dest}",
                manifest_path=str(manifest_path),
                dependency_name=name,
                dependency_type="archive",
            )
        verification: dict[str, object] = {"extracted": True}
        if expected_sha256 or expected_size is not None:
            verification.update(
                _verify_file_artifact(
                    archive_path,
                    expected_size=expected_size,
                    expected_sha256=expected_sha256,
                    manifest_path=manifest_path,
                    dependency_name=name,
                    dependency_type="archive",
                )
            )
        return DependencyResult(
            name=name,
            exports=exports,
            state_entry=_record_state_entry(
                name,
                "archive",
                manifest_path,
                "verified",
                inputs=desired_inputs,
                resolved=resolved_version,
                outputs=outputs,
                verification=verification,
                exports=exports,
            ),
        )

    satisfied = False
    verification: dict[str, object] | None = None
    if not ctx.update_only and not _should_refresh(ctx, name) and extract_dest.exists():
        if _dependency_inputs_match(current_entry, desired_inputs) or current_entry is None:
            satisfied = True
            verification = {"extracted": True}
            if archive_path.exists() and (expected_sha256 or expected_size is not None):
                verification.update(
                    _verify_file_artifact(
                        archive_path,
                        expected_size=expected_size,
                        expected_sha256=expected_sha256,
                        manifest_path=manifest_path,
                        dependency_name=name,
                        dependency_type="archive",
                    )
                )

    if satisfied:
        ctx.printer.step("SKIP", f"archive {name} -> {extract_dest}")  # type: ignore[union-attr]
        return DependencyResult(
            name=name,
            exports=exports,
            state_entry=_record_state_entry(
                name,
                "archive",
                manifest_path,
                "present",
                inputs=desired_inputs,
                resolved=resolved_version,
                outputs=outputs,
                verification=verification,
                exports=exports,
            ),
            skipped=True,
        )

    phase = "REFRESH" if (extract_dest.exists() or _should_refresh(ctx, name) or ctx.update_only) else "FETCH"
    ctx.printer.step(phase, f"archive {name} -> {extract_dest}")  # type: ignore[union-attr]
    final_url = _download_to_file(
        url,
        archive_path,
        manifest_path=manifest_path,
        dependency_name=name,
        dependency_type="archive",
        printer=ctx.printer,
        dry_run=ctx.dry_run,
    )

    if ctx.dry_run:
        verification = {}
    else:
        verification = {}
        if expected_sha256 or expected_size is not None:
            verification.update(
                _verify_file_artifact(
                    archive_path,
                    expected_size=expected_size,
                    expected_sha256=expected_sha256,
                    manifest_path=manifest_path,
                    dependency_name=name,
                    dependency_type="archive",
                )
            )

        if extract_dest.exists():
            shutil.rmtree(extract_dest)
        temp_extract_dir.parent.mkdir(parents=True, exist_ok=True)
        shutil.rmtree(temp_extract_dir, ignore_errors=True)
        temp_extract_dir.mkdir(parents=True, exist_ok=True)

        if strip_components > 0:
            if expected_root or rename_root_to:
                raise DependencyExecutionError(
                    "strip_components cannot be combined with expected_root or rename_root_to",
                    manifest_path=str(manifest_path),
                    dependency_name=name,
                    dependency_type="archive",
                )
            _extract_archive_with_strip(archive_path, extract_dest, strip_components)
        else:
            if zipfile.is_zipfile(archive_path):
                with zipfile.ZipFile(archive_path, "r") as archive:
                    archive.extractall(temp_extract_dir)
            else:
                with tarfile.open(archive_path, "r:*") as archive:
                    archive.extractall(temp_extract_dir)

            if rename_root_to:
                source_root = temp_extract_dir / expected_root if expected_root else None
                if source_root is None or not source_root.exists():
                    children = list(temp_extract_dir.iterdir())
                    if len(children) == 1:
                        source_root = children[0]
                if source_root is None or not source_root.exists():
                    raise DependencyExecutionError(
                        "rename_root_to requires expected_root or a single top-level extracted entry",
                        manifest_path=str(manifest_path),
                        dependency_name=name,
                        dependency_type="archive",
                    )
                source_root.rename(temp_extract_dir / rename_root_to)
                expected_root = rename_root_to

            if expected_root and not (temp_extract_dir / expected_root).exists():
                raise DependencyExecutionError(
                    f"expected extracted root '{expected_root}' was not found",
                    manifest_path=str(manifest_path),
                    dependency_name=name,
                    dependency_type="archive",
                )

            _copy_tree_contents(temp_extract_dir, extract_dest)
        shutil.rmtree(temp_extract_dir, ignore_errors=True)
        verification["extracted"] = True

    resolved_state = dict(resolved_version)
    resolved_state["url"] = url
    resolved_state["final_url"] = final_url
    return DependencyResult(
        name=name,
        exports=exports,
        state_entry=_record_state_entry(
            name,
            "archive",
            manifest_path,
            "extracted" if not ctx.dry_run else "planned",
            inputs=desired_inputs,
            resolved=resolved_state,
            outputs=outputs,
            verification=verification,
            exports=exports,
        ),
    )


def _execute_local(ctx: "RunContext", dependency: dict, manifest_path: Path, current_entry: dict | None) -> DependencyResult:
    del current_entry

    name = dependency["name"]
    path_value = resolve_string(dependency["path"], ctx.variables, str(manifest_path), f"dependencies.{name}.path")
    path = Path(path_value)
    if not path.is_absolute():
        path = (manifest_path.parent / path).resolve()
    if not path.exists():
        raise DependencyExecutionError(
            f"local dependency path does not exist: {path}",
            manifest_path=str(manifest_path),
            dependency_name=name,
            dependency_type="local",
        )
    phase = "VERIFY" if ctx.verify_only else "RUN"
    ctx.printer.step(phase, f"local {name} -> {path}")  # type: ignore[union-attr]
    exports = _default_root_export({"name": name, "type": "local", "path": str(path)}, manifest_path)
    exports.update(_resolve_exports(ctx, {**dependency, "path": str(path)}, manifest_path, f"dependencies.{name}"))
    return DependencyResult(
        name=name,
        exports=exports,
        state_entry=_record_state_entry(
            name,
            "local",
            manifest_path,
            "verified" if ctx.verify_only else "present",
            inputs={"path": str(path)},
            outputs={"dest": str(path)},
            verification={"exists": True},
            exports=exports,
        ),
    )


def _execute_tool(ctx: "RunContext", dependency: dict, manifest_path: Path, current_entry: dict | None) -> DependencyResult:
    name = dependency["name"]
    source_dependency = dict(dependency["source"])
    source_dependency["name"] = name
    if "exports" not in source_dependency:
        source_dependency["exports"] = dependency.get("exports", {})

    dep_type = source_dependency["type"]
    if dep_type == "git":
        result = _execute_git(ctx, source_dependency, manifest_path, current_entry)
    elif dep_type == "archive":
        result = _execute_archive(ctx, source_dependency, manifest_path, current_entry)
    elif dep_type == "file":
        result = _execute_file(ctx, source_dependency, manifest_path, current_entry)
    elif dep_type == "local":
        result = _execute_local(ctx, source_dependency, manifest_path, current_entry)
    else:
        raise DependencyExecutionError(
            f"unsupported tool source type '{dep_type}'",
            manifest_path=str(manifest_path),
            dependency_name=name,
            dependency_type="tool",
        )
    if result.state_entry is not None:
        result.state_entry["type"] = "tool"
    return result


def execute_dependency(ctx: "RunContext", dependency: dict, manifest_path: Path, field: str | None = None) -> DependencyResult:
    field_name = field or f"dependencies.{dependency['name']}"
    if dependency["name"] in ctx.seen_dependency_names:
        raise ManifestValidationError(
            f"duplicate dependency name '{dependency['name']}' across manifests",
            manifest_path=str(manifest_path),
            field=field_name,
        )

    enabled = item_enabled(
        dependency,
        ctx.default_platform,
        ctx.variables,
        str(manifest_path),
        field_name,
        manifest_path.parent,
    )
    if not enabled:
        ctx.seen_dependency_names.add(dependency["name"])
        result = _dependency_skipped(ctx, dependency, manifest_path)
        for export_name in result.exports:
            if export_name in ctx.seen_export_names:
                raise ManifestValidationError(
                    f"duplicate export name '{export_name}' across manifests",
                    manifest_path=str(manifest_path),
                    field=f"{field_name}.exports",
                )
            ctx.seen_export_names.add(export_name)
        return result

    current_entry = ctx.state["dependencies"].get(dependency["name"])
    if ctx.clean_targets:
        if _should_clean(ctx, dependency["name"]):
            result = _clean_dependency_outputs(ctx, dependency, current_entry, manifest_path)
        else:
            result = _preserve_existing_result(dependency["name"], current_entry, "clean mode", ctx)
    else:
        dep_type = dependency["type"]
        if dep_type == "git":
            result = _execute_git(ctx, dependency, manifest_path, current_entry)
        elif dep_type == "archive":
            result = _execute_archive(ctx, dependency, manifest_path, current_entry)
        elif dep_type == "file":
            result = _execute_file(ctx, dependency, manifest_path, current_entry)
        elif dep_type == "local":
            result = _execute_local(ctx, dependency, manifest_path, current_entry)
        elif dep_type == "tool":
            result = _execute_tool(ctx, dependency, manifest_path, current_entry)
        else:
            raise DependencyExecutionError(
                f"unhandled dependency type '{dep_type}'",
                manifest_path=str(manifest_path),
                dependency_name=dependency["name"],
                dependency_type=dep_type,
            )

    ctx.seen_dependency_names.add(dependency["name"])
    for export_name in result.exports:
        if export_name in ctx.seen_export_names:
            raise ManifestValidationError(
                f"duplicate export name '{export_name}' across manifests",
                manifest_path=str(manifest_path),
                field=f"{field_name}.exports",
            )
        ctx.seen_export_names.add(export_name)
    return result
