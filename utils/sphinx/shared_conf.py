from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, MutableMapping


def _append_unique(items: list[str], value: str) -> None:
    if value not in items:
        items.append(value)


def _resolve_repo_root(docs_dir: Path) -> Path:
    candidates = (docs_dir.parent, docs_dir.parent.parent)
    for candidate in candidates:
        if (candidate / "utils" / "sphinx" / "static").is_dir():
            return candidate

    raise RuntimeError(
        f"Could not determine repository root from docs directory {docs_dir}. "
        "Expected shared Sphinx assets at either the docs parent or grandparent directory."
    )


def _load_global_nav() -> list[dict[str, Any]]:
    raw_value = os.environ.get("OMEGA_SPHINX_NAV_JSON")
    if not raw_value:
        return []

    try:
        data = json.loads(raw_value)
    except json.JSONDecodeError as exc:
        raise RuntimeError("OMEGA_SPHINX_NAV_JSON contained invalid JSON.") from exc

    if not isinstance(data, list):
        raise RuntimeError("OMEGA_SPHINX_NAV_JSON must decode to a list.")

    nav_items: list[dict[str, Any]] = []
    for entry in data:
        if not isinstance(entry, dict):
            raise RuntimeError("OMEGA_SPHINX_NAV_JSON entries must be objects.")
        title = entry.get("title")
        path = entry.get("path")
        current = entry.get("current", False)
        if not isinstance(title, str) or not isinstance(path, str) or not isinstance(current, bool):
            raise RuntimeError("OMEGA_SPHINX_NAV_JSON entries must include string title/path and bool current.")
        nav_items.append({"title": title, "path": path, "current": current})

    return nav_items


def apply_shared_sphinx_style(config: MutableMapping[str, Any], docs_dir: Path) -> None:
    docs_dir = Path(docs_dir).resolve()
    repo_root = _resolve_repo_root(docs_dir)
    shared_root = repo_root / "utils" / "sphinx"
    shared_static_dir = shared_root / "static"
    shared_theme_name = "omegagraphics"
    shared_theme_dir = shared_root / shared_theme_name
    if not shared_static_dir.is_dir():
        raise RuntimeError(
            f"Could not find shared Sphinx assets at {shared_static_dir}. "
            "Expected the Omega Graphics Project utils/sphinx/static directory."
        )
    if not shared_theme_dir.is_dir():
        raise RuntimeError(
            f"Could not find the shared Sphinx theme at {shared_theme_dir}. "
            "Expected the Omega Graphics Project utils/sphinx/omegagraphics directory."
        )

    html_static_path = list(config.get("html_static_path", []))
    _append_unique(html_static_path, str(shared_static_dir))
    config["html_static_path"] = html_static_path

    html_theme_path = list(config.get("html_theme_path", []))
    _append_unique(html_theme_path, str(shared_root))
    config["html_theme_path"] = html_theme_path

    config["html_theme"] = shared_theme_name

    html_context = dict(config.get("html_context", {}))
    html_context.setdefault("omega_global_nav", _load_global_nav())
    config["html_context"] = html_context
