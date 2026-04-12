from __future__ import annotations

import json
import os
from pathlib import Path


def automdeps_dir(root_dir: str | Path) -> Path:
    return Path(root_dir) / ".automdeps"


def state_path(root_dir: str | Path) -> Path:
    return automdeps_dir(root_dir) / "state.json"


def exports_path(root_dir: str | Path) -> Path:
    return automdeps_dir(root_dir) / "exports.json"


def load_state(root_dir: str | Path) -> dict:
    path = state_path(root_dir)
    if not path.exists():
        return {"dependencies": {}}
    with path.open("r", encoding="utf-8") as stream:
        data = json.load(stream)
    if not isinstance(data, dict):
        return {"dependencies": {}}
    if "dependencies" not in data or not isinstance(data["dependencies"], dict):
        data["dependencies"] = {}
    return data


def exports_from_state(state: dict) -> dict[str, str]:
    dependencies = state.get("dependencies", {})
    if not isinstance(dependencies, dict):
        return {}

    exports: dict[str, str] = {}
    for entry in dependencies.values():
        if not isinstance(entry, dict):
            continue
        entry_exports = entry.get("exports", {})
        if not isinstance(entry_exports, dict):
            continue
        for key, value in entry_exports.items():
            if isinstance(key, str) and isinstance(value, str):
                exports[key] = value
    return exports


def _write_json_atomic(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp_path = path.with_suffix(path.suffix + ".tmp")
    with temp_path.open("w", encoding="utf-8") as stream:
        json.dump(payload, stream, indent=2, sort_keys=True)
        stream.write("\n")
    os.replace(temp_path, path)


def save_state(root_dir: str | Path, state: dict) -> None:
    _write_json_atomic(state_path(root_dir), state)


def save_exports(root_dir: str | Path, exports: dict[str, str]) -> None:
    _write_json_atomic(exports_path(root_dir), exports)
