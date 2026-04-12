from __future__ import annotations

from pathlib import Path
from typing import Any, MutableMapping


def _append_unique(items: list[str], value: str) -> None:
    if value not in items:
        items.append(value)


def apply_shared_sphinx_style(config: MutableMapping[str, Any], docs_dir: Path) -> None:
    docs_dir = Path(docs_dir).resolve()
    repo_root = docs_dir.parent.parent
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
