from __future__ import annotations

import os
import platform
from pathlib import Path

from .variables import resolve_string


def normalize_arch() -> str:
    machine = platform.machine().lower()
    aliases = {
        "amd64": "x86_64",
        "x64": "x86_64",
        "arm64": "arm64",
        "aarch64": "arm64",
    }
    return aliases.get(machine, machine)


def _match_platforms(item: dict, default_platform: str) -> bool:
    platforms = item.get("platforms")
    if platforms and default_platform not in platforms:
        return False
    return True


def _match_when(item: dict, default_platform: str, variables: dict[str, object], manifest_path: str, field: str, cwd: str) -> bool:
    when = item.get("when")
    if not when:
        return True

    if "platform" in when and default_platform not in when["platform"]:
        return False

    current_arch = normalize_arch()
    if "arch" in when and current_arch not in [value.lower() for value in when["arch"]]:
        return False

    for index, path_value in enumerate(when.get("exists", [])):
        resolved = resolve_string(path_value, variables, manifest_path, f"{field}.when.exists[{index}]")
        if not os.path.exists(os.path.join(cwd, resolved)):
            return False

    for index, path_value in enumerate(when.get("not_exists", [])):
        resolved = resolve_string(path_value, variables, manifest_path, f"{field}.when.not_exists[{index}]")
        if os.path.exists(os.path.join(cwd, resolved)):
            return False

    return True


def item_enabled(item: dict, default_platform: str, variables: dict[str, object], manifest_path: str, field: str, cwd: str | Path) -> bool:
    cwd_str = str(cwd)
    return _match_platforms(item, default_platform) and _match_when(
        item,
        default_platform,
        variables,
        manifest_path,
        field,
        cwd_str,
    )
