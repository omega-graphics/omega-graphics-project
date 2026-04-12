from __future__ import annotations

import re
from pathlib import Path

import requests

from .errors import DependencyExecutionError


STABLE_TAG_MARKERS = ("alpha", "beta", "rc", "pre", "preview", "nightly", "dev")
GITHUB_REPO_PATTERN = re.compile(r"github\.com[/:]([^/]+/[^/.]+?)(?:\.git)?$")


def _github_repo_from_url(url: str | None) -> str | None:
    if not url:
        return None
    match = GITHUB_REPO_PATTERN.search(url)
    if not match:
        return None
    return match.group(1)


def _stable_version_from_tag(tag_name: str) -> str:
    version = tag_name.strip()
    if version.lower().startswith("v") and len(version) > 1 and version[1].isdigit():
        return version[1:]
    return version


def _is_stable_release(release: dict) -> bool:
    if release.get("draft") or release.get("prerelease"):
        return False
    tag_name = str(release.get("tag_name", ""))
    lowered = tag_name.lower()
    return not any(marker in lowered for marker in STABLE_TAG_MARKERS)


def _first_number(value: str) -> str:
    match = re.search(r"(\d+)", value)
    if not match:
        raise ValueError(f"no numeric version component found in '{value}'")
    return match.group(1)


def _github_json(url: str, dependency_name: str, dependency_type: str) -> dict | list:
    try:
        response = requests.get(
            url,
            timeout=(15, 60),
            headers={
                "Accept": "application/vnd.github+json",
                "User-Agent": "autom-deps/1.0",
            },
        )
        response.raise_for_status()
        return response.json()
    except requests.RequestException as ex:
        raise DependencyExecutionError(
            f"stable version resolution failed: {ex}",
            dependency_name=dependency_name,
            dependency_type=dependency_type,
        ) from ex


def _resolve_repo(version_source: dict, dependency: dict) -> str:
    repo = version_source.get("repo")
    if isinstance(repo, str) and repo:
        return repo
    inferred_repo = _github_repo_from_url(dependency.get("url"))
    if inferred_repo:
        return inferred_repo
    raise DependencyExecutionError(
        "stable version resolution requires a GitHub repo or inferable GitHub URL",
        dependency_name=dependency["name"],
        dependency_type=dependency["type"],
    )


def _latest_stable_release(repo: str, dependency_name: str, dependency_type: str) -> dict:
    releases = _github_json(
        f"https://api.github.com/repos/{repo}/releases",
        dependency_name,
        dependency_type,
    )
    if not isinstance(releases, list):
        raise DependencyExecutionError(
            "stable version resolution returned an unexpected releases payload",
            dependency_name=dependency_name,
            dependency_type=dependency_type,
        )
    for release in releases:
        if isinstance(release, dict) and _is_stable_release(release):
            return release
    raise DependencyExecutionError(
        f"no stable release found for GitHub repo '{repo}'",
        dependency_name=dependency_name,
        dependency_type=dependency_type,
    )


def _resolve_major(major_source: dict, dependency: dict) -> tuple[str, str | None]:
    source_type = major_source["type"]
    if source_type == "github-releases":
        release = _latest_stable_release(major_source["repo"], dependency["name"], dependency["type"])
        tag_name = str(release.get("tag_name", ""))
        version = _stable_version_from_tag(tag_name)
        return _first_number(version), version

    manual_value = str(major_source.get("value", major_source.get("version", "")))
    return _first_number(manual_value), manual_value or None


def resolve_version_source(version_source: dict, dependency: dict) -> dict[str, str]:
    strategy = version_source["strategy"]
    dependency_name = dependency["name"]
    dependency_type = dependency["type"]

    if strategy == "manual":
        return {
            key: str(version_source[key])
            for key in ("version", "ref", "url")
            if key in version_source
        }

    if strategy == "git-default-branch":
        repo = _resolve_repo(version_source, dependency)
        repo_info = _github_json(f"https://api.github.com/repos/{repo}", dependency_name, dependency_type)
        if not isinstance(repo_info, dict) or "default_branch" not in repo_info:
            raise DependencyExecutionError(
                f"failed to resolve default branch for GitHub repo '{repo}'",
                dependency_name=dependency_name,
                dependency_type=dependency_type,
            )
        return {"ref": str(repo_info["default_branch"]), "repo": repo}

    if strategy == "git-tag-latest-stable":
        repo = _resolve_repo(version_source, dependency)
        release = _latest_stable_release(repo, dependency_name, dependency_type)
        tag_name = str(release.get("tag_name", ""))
        return {
            "repo": repo,
            "ref": tag_name,
            "version": _stable_version_from_tag(tag_name),
        }

    if strategy == "git-branch-pattern":
        repo = _resolve_repo(version_source, dependency)
        major, upstream_version = _resolve_major(version_source["major_source"], dependency)
        ref = str(version_source["pattern"]).format(major=major, version=upstream_version or major)
        result = {"repo": repo, "ref": ref, "major": major}
        if upstream_version:
            result["version"] = upstream_version
        return result

    if strategy == "github-releases":
        repo = _resolve_repo(version_source, dependency)
        release = _latest_stable_release(repo, dependency_name, dependency_type)
        tag_name = str(release.get("tag_name", ""))
        version = _stable_version_from_tag(tag_name)
        resolved = {"repo": repo, "tag": tag_name, "version": version}
        asset_template = version_source.get("asset_template")
        if isinstance(asset_template, str):
            asset_name = asset_template.format(version=version, tag=tag_name)
            assets = release.get("assets", [])
            if not isinstance(assets, list):
                assets = []
            for asset in assets:
                if not isinstance(asset, dict):
                    continue
                if asset.get("name") == asset_name and asset.get("browser_download_url"):
                    resolved["url"] = str(asset["browser_download_url"])
                    resolved["asset"] = asset_name
                    return resolved
            raise DependencyExecutionError(
                f"release asset '{asset_name}' was not found for GitHub repo '{repo}'",
                dependency_name=dependency_name,
                dependency_type=dependency_type,
            )
        archive_url = release.get("zipball_url") or release.get("tarball_url")
        if not archive_url:
            raise DependencyExecutionError(
                f"stable GitHub release for '{repo}' did not provide a downloadable archive",
                dependency_name=dependency_name,
                dependency_type=dependency_type,
            )
        resolved["url"] = str(archive_url)
        return resolved

    if strategy == "url-template":
        repo = _resolve_repo(version_source, dependency)
        release = _latest_stable_release(repo, dependency_name, dependency_type)
        tag_name = str(release.get("tag_name", ""))
        version = _stable_version_from_tag(tag_name)
        url = str(version_source["url_template"]).format(version=version, tag=tag_name)
        return {
            "repo": repo,
            "tag": tag_name,
            "version": version,
            "url": url,
        }

    raise DependencyExecutionError(
        f"unhandled version_source strategy '{strategy}'",
        dependency_name=dependency_name,
        dependency_type=dependency_type,
    )


def apply_resolved_version_source(dependency: dict, resolved: dict[str, str]) -> dict:
    updated = dict(dependency)
    if updated["type"] == "git" and "ref" in resolved:
        updated["ref"] = resolved["ref"]
    if updated["type"] in {"archive", "file"} and "url" in resolved:
        updated["url"] = resolved["url"]
    return updated


def cleaned_paths_from_state(entry: dict) -> list[Path]:
    outputs = entry.get("outputs", {})
    if not isinstance(outputs, dict):
        return []

    paths: list[Path] = []
    for key in ("dest", "cache_dir", "temp_dir", "archive_path"):
        value = outputs.get(key)
        if isinstance(value, str) and value:
            paths.append(Path(value))
    return paths
