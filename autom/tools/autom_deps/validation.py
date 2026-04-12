from __future__ import annotations

from .errors import ManifestValidationError


TOP_LEVEL_KEYS = {
    "variables",
    "dependencies",
    "commands",
    "rootCommands",
    "postCommands",
    "postRootCommands",
    "subdirs",
}

SUPPORTED_COMMAND_TYPES = {
    "git_clone": {"url", "dest", "branch"},
    "clone": {"url", "dest", "branch"},
    "chdir": {"dir"},
    "system": {"path"},
    "script": {"path", "args"},
    "download": {"url", "dest"},
    "tar": {"tarfile", "dest"},
    "unzip": {"zipfile", "dest"},
}

COMMON_COMMAND_KEYS = {"type", "platforms", "when"}
SUPPORTED_PLATFORMS = {"windows", "macos", "linux", "ios", "android"}
SUPPORTED_WHEN_KEYS = {"platform", "arch", "exists", "not_exists"}
SUPPORTED_DEPENDENCY_TYPES = {"git", "archive", "file", "tool", "local"}
COMMON_DEPENDENCY_KEYS = {"name", "type", "platforms", "when", "exports", "version_source"}
DEPENDENCY_TYPE_KEYS = {
    "git": {"url", "dest", "ref"},
    "archive": {
        "url",
        "dest",
        "archive_name",
        "strip_components",
        "dest_subdir",
        "expected_root",
        "rename_root_to",
        "sha256",
        "size",
    },
    "file": {"url", "dest", "sha256", "size"},
    "tool": {"source"},
    "local": {"path"},
}
REQUIRED_DEPENDENCY_KEYS = {
    "git": {"url", "dest"},
    "archive": {"url", "dest"},
    "file": {"url", "dest"},
    "tool": {"source"},
    "local": {"path"},
}
SUPPORTED_VERSION_SOURCE_STRATEGIES = {
    "git-default-branch",
    "git-tag-latest-stable",
    "git-branch-pattern",
    "github-releases",
    "url-template",
    "manual",
}
SUPPORTED_VERSION_SOURCE_CHANNELS = {"stable"}
SUPPORTED_MAJOR_SOURCE_TYPES = {"github-releases", "manual"}


def _expect_type(value, expected_type, manifest_path: str, field: str) -> None:
    if not isinstance(value, expected_type):
        expected_name = expected_type.__name__
        raise ManifestValidationError(
            f"expected {expected_name}, got {type(value).__name__}",
            manifest_path=manifest_path,
            field=field,
        )


def _validate_platforms(command: dict, manifest_path: str, field: str) -> None:
    platforms = command.get("platforms")
    if platforms is None:
        return
    _expect_type(platforms, list, manifest_path, field)
    for index, platform_name in enumerate(platforms):
        _expect_type(platform_name, str, manifest_path, f"{field}[{index}]")
        if platform_name not in SUPPORTED_PLATFORMS:
            raise ManifestValidationError(
                f"unsupported platform '{platform_name}'",
                manifest_path=manifest_path,
                field=f"{field}[{index}]",
            )


def _validate_when(when, manifest_path: str, field: str) -> None:
    if when is None:
        return
    _expect_type(when, dict, manifest_path, field)
    unknown_keys = sorted(set(when.keys()) - SUPPORTED_WHEN_KEYS)
    if unknown_keys:
        raise ManifestValidationError(
            f"unknown when keys: {', '.join(unknown_keys)}",
            manifest_path=manifest_path,
            field=field,
        )
    for key, value in when.items():
        _expect_type(value, list, manifest_path, f"{field}.{key}")
        for index, item in enumerate(value):
            _expect_type(item, str, manifest_path, f"{field}.{key}[{index}]")


def _validate_exports(exports, manifest_path: str, field: str) -> None:
    if exports is None:
        return
    _expect_type(exports, dict, manifest_path, field)
    seen = set()
    for key, value in exports.items():
        _expect_type(key, str, manifest_path, f"{field}.key")
        _expect_type(value, str, manifest_path, f"{field}.{key}")
        if key in seen:
            raise ManifestValidationError(
                f"duplicate export key '{key}'",
                manifest_path=manifest_path,
                field=field,
            )
        seen.add(key)


def _validate_major_source(major_source, manifest_path: str, field: str) -> None:
    _expect_type(major_source, dict, manifest_path, field)
    source_type = major_source.get("type")
    if not isinstance(source_type, str):
        raise ManifestValidationError(
            "major_source is missing string 'type'",
            manifest_path=manifest_path,
            field=f"{field}.type",
        )
    if source_type not in SUPPORTED_MAJOR_SOURCE_TYPES:
        raise ManifestValidationError(
            f"unsupported major_source type '{source_type}'",
            manifest_path=manifest_path,
            field=f"{field}.type",
        )

    allowed_keys = {"type", "repo", "value", "version"}
    unknown_keys = sorted(set(major_source.keys()) - allowed_keys)
    if unknown_keys:
        raise ManifestValidationError(
            f"unknown major_source keys: {', '.join(unknown_keys)}",
            manifest_path=manifest_path,
            field=field,
        )

    if source_type == "github-releases":
        if "repo" not in major_source:
            raise ManifestValidationError(
                "major_source type 'github-releases' requires 'repo'",
                manifest_path=manifest_path,
                field=field,
            )
        _expect_type(major_source["repo"], str, manifest_path, f"{field}.repo")
        return

    value_key = "value" if "value" in major_source else "version"
    if value_key not in major_source:
        raise ManifestValidationError(
            "major_source type 'manual' requires 'value' or 'version'",
            manifest_path=manifest_path,
            field=field,
        )
    _expect_type(major_source[value_key], str, manifest_path, f"{field}.{value_key}")


def _validate_version_source(version_source, manifest_path: str, field: str, dependency_type: str) -> None:
    if version_source is None:
        return
    _expect_type(version_source, dict, manifest_path, field)

    strategy = version_source.get("strategy")
    if not isinstance(strategy, str):
        raise ManifestValidationError(
            "version_source is missing string 'strategy'",
            manifest_path=manifest_path,
            field=f"{field}.strategy",
        )
    if strategy not in SUPPORTED_VERSION_SOURCE_STRATEGIES:
        raise ManifestValidationError(
            f"unsupported version_source strategy '{strategy}'",
            manifest_path=manifest_path,
            field=f"{field}.strategy",
        )

    channel = version_source.get("channel")
    if not isinstance(channel, str):
        raise ManifestValidationError(
            "version_source is missing string 'channel'",
            manifest_path=manifest_path,
            field=f"{field}.channel",
        )
    if channel not in SUPPORTED_VERSION_SOURCE_CHANNELS:
        raise ManifestValidationError(
            f"unsupported version_source channel '{channel}'",
            manifest_path=manifest_path,
            field=f"{field}.channel",
        )

    allowed_keys = {"channel", "strategy", "repo"}
    if strategy == "git-branch-pattern":
        allowed_keys |= {"pattern", "major_source"}
    elif strategy == "github-releases":
        allowed_keys |= {"asset_template"}
    elif strategy == "url-template":
        allowed_keys |= {"url_template"}
    elif strategy == "manual":
        allowed_keys |= {"version", "ref", "url"}

    unknown_keys = sorted(set(version_source.keys()) - allowed_keys)
    if unknown_keys:
        raise ManifestValidationError(
            f"unknown version_source keys: {', '.join(unknown_keys)}",
            manifest_path=manifest_path,
            field=field,
        )

    if "repo" in version_source:
        _expect_type(version_source["repo"], str, manifest_path, f"{field}.repo")

    if strategy == "git-default-branch":
        if dependency_type != "git":
            raise ManifestValidationError(
                "git-default-branch is only supported on git dependencies",
                manifest_path=manifest_path,
                field=field,
            )
        return

    if strategy == "git-tag-latest-stable":
        if dependency_type != "git":
            raise ManifestValidationError(
                "git-tag-latest-stable is only supported on git dependencies",
                manifest_path=manifest_path,
                field=field,
            )
        return

    if strategy == "git-branch-pattern":
        if dependency_type != "git":
            raise ManifestValidationError(
                "git-branch-pattern is only supported on git dependencies",
                manifest_path=manifest_path,
                field=field,
            )
        if "pattern" not in version_source:
            raise ManifestValidationError(
                "git-branch-pattern requires 'pattern'",
                manifest_path=manifest_path,
                field=field,
            )
        _expect_type(version_source["pattern"], str, manifest_path, f"{field}.pattern")
        if "major_source" not in version_source:
            raise ManifestValidationError(
                "git-branch-pattern requires 'major_source'",
                manifest_path=manifest_path,
                field=field,
            )
        _validate_major_source(version_source["major_source"], manifest_path, f"{field}.major_source")
        return

    if strategy == "github-releases":
        if dependency_type not in {"archive", "file"}:
            raise ManifestValidationError(
                "github-releases is only supported on archive and file dependencies",
                manifest_path=manifest_path,
                field=field,
            )
        if "repo" not in version_source:
            raise ManifestValidationError(
                "github-releases requires 'repo'",
                manifest_path=manifest_path,
                field=field,
            )
        if "asset_template" in version_source:
            _expect_type(version_source["asset_template"], str, manifest_path, f"{field}.asset_template")
        return

    if strategy == "url-template":
        if dependency_type not in {"archive", "file"}:
            raise ManifestValidationError(
                "url-template is only supported on archive and file dependencies",
                manifest_path=manifest_path,
                field=field,
            )
        if "repo" not in version_source:
            raise ManifestValidationError(
                "url-template requires 'repo'",
                manifest_path=manifest_path,
                field=field,
            )
        if "url_template" not in version_source:
            raise ManifestValidationError(
                "url-template requires 'url_template'",
                manifest_path=manifest_path,
                field=field,
            )
        _expect_type(version_source["url_template"], str, manifest_path, f"{field}.url_template")
        return

    manual_keys = [key for key in ("version", "ref", "url") if key in version_source]
    if not manual_keys:
        raise ManifestValidationError(
            "manual version_source requires at least one of 'version', 'ref', or 'url'",
            manifest_path=manifest_path,
            field=field,
        )
    for key in manual_keys:
        _expect_type(version_source[key], str, manifest_path, f"{field}.{key}")


def validate_command(command: dict, manifest_path: str, field: str) -> None:
    _expect_type(command, dict, manifest_path, field)
    command_type = command.get("type")
    if not isinstance(command_type, str):
        raise ManifestValidationError(
            "command is missing string 'type'",
            manifest_path=manifest_path,
            field=field,
        )
    if command_type not in SUPPORTED_COMMAND_TYPES:
        raise ManifestValidationError(
            f"unsupported command type '{command_type}'",
            manifest_path=manifest_path,
            field=f"{field}.type",
        )

    allowed_keys = COMMON_COMMAND_KEYS | SUPPORTED_COMMAND_TYPES[command_type]
    unknown_keys = sorted(set(command.keys()) - allowed_keys)
    if unknown_keys:
        raise ManifestValidationError(
            f"unknown command keys: {', '.join(unknown_keys)}",
            manifest_path=manifest_path,
            field=field,
        )

    for required_key in SUPPORTED_COMMAND_TYPES[command_type]:
        if required_key not in command:
            raise ManifestValidationError(
                f"missing required key '{required_key}'",
                manifest_path=manifest_path,
                field=field,
            )

    _validate_platforms(command, manifest_path, f"{field}.platforms")
    _validate_when(command.get("when"), manifest_path, f"{field}.when")

    for key in SUPPORTED_COMMAND_TYPES[command_type]:
        if command_type == "script" and key == "args":
            _expect_type(command[key], list, manifest_path, f"{field}.{key}")
            for index, arg in enumerate(command[key]):
                _expect_type(arg, str, manifest_path, f"{field}.{key}[{index}]")
        else:
            _expect_type(command[key], str, manifest_path, f"{field}.{key}")


def validate_dependency(dependency: dict, manifest_path: str, field: str, *, nested: bool = False) -> None:
    _expect_type(dependency, dict, manifest_path, field)
    dependency_type = dependency.get("type")
    if not isinstance(dependency_type, str):
        raise ManifestValidationError(
            "dependency is missing string 'type'",
            manifest_path=manifest_path,
            field=f"{field}.type",
        )
    if dependency_type not in SUPPORTED_DEPENDENCY_TYPES:
        raise ManifestValidationError(
            f"unsupported dependency type '{dependency_type}'",
            manifest_path=manifest_path,
            field=f"{field}.type",
        )
    if nested and dependency_type == "tool":
        raise ManifestValidationError(
            "nested tool dependencies are not supported",
            manifest_path=manifest_path,
            field=f"{field}.type",
        )

    if not nested:
        name = dependency.get("name")
        if not isinstance(name, str):
            raise ManifestValidationError(
                "dependency is missing string 'name'",
                manifest_path=manifest_path,
                field=f"{field}.name",
            )

    allowed_keys = COMMON_DEPENDENCY_KEYS | DEPENDENCY_TYPE_KEYS[dependency_type]
    if nested:
        allowed_keys = (allowed_keys - {"name", "exports", "platforms", "when"}) | {"exports"}
    unknown_keys = sorted(set(dependency.keys()) - allowed_keys)
    if unknown_keys:
        raise ManifestValidationError(
            f"unknown dependency keys: {', '.join(unknown_keys)}",
            manifest_path=manifest_path,
            field=field,
        )

    if not nested:
        _validate_platforms(dependency, manifest_path, f"{field}.platforms")
        _validate_when(dependency.get("when"), manifest_path, f"{field}.when")
    _validate_exports(dependency.get("exports"), manifest_path, f"{field}.exports")
    _validate_version_source(dependency.get("version_source"), manifest_path, f"{field}.version_source", dependency_type)

    if dependency_type == "tool":
        if "source" not in dependency:
            raise ManifestValidationError(
                "missing required key 'source'",
                manifest_path=manifest_path,
                field=field,
            )
        validate_dependency(dependency["source"], manifest_path, f"{field}.source", nested=True)
        return

    for required_key in REQUIRED_DEPENDENCY_KEYS[dependency_type]:
        if required_key not in dependency:
            raise ManifestValidationError(
                f"missing required key '{required_key}'",
                manifest_path=manifest_path,
                field=field,
            )

    if dependency_type == "archive":
        for key in ("url", "dest", "archive_name", "dest_subdir", "expected_root", "rename_root_to", "sha256"):
            if key in dependency:
                _expect_type(dependency[key], str, manifest_path, f"{field}.{key}")
        if "size" in dependency:
            _expect_type(dependency["size"], int, manifest_path, f"{field}.size")
            if dependency["size"] < 0:
                raise ManifestValidationError(
                    "size must be >= 0",
                    manifest_path=manifest_path,
                    field=f"{field}.size",
                )
        if "strip_components" in dependency:
            _expect_type(dependency["strip_components"], int, manifest_path, f"{field}.strip_components")
            if dependency["strip_components"] < 0:
                raise ManifestValidationError(
                    "strip_components must be >= 0",
                    manifest_path=manifest_path,
                    field=f"{field}.strip_components",
                )
        return

    if dependency_type == "file":
        for key in DEPENDENCY_TYPE_KEYS[dependency_type]:
            if key == "size" and key in dependency:
                _expect_type(dependency[key], int, manifest_path, f"{field}.{key}")
                if dependency[key] < 0:
                    raise ManifestValidationError(
                        "size must be >= 0",
                        manifest_path=manifest_path,
                        field=f"{field}.{key}",
                    )
            elif key in dependency:
                _expect_type(dependency[key], str, manifest_path, f"{field}.{key}")
        return

    if dependency_type == "git":
        for key in DEPENDENCY_TYPE_KEYS[dependency_type]:
            if key in dependency:
                _expect_type(dependency[key], str, manifest_path, f"{field}.{key}")
        return

    if dependency_type == "local":
        _expect_type(dependency["path"], str, manifest_path, f"{field}.path")
        return


def validate_manifest(manifest: dict, manifest_path: str) -> None:
    _expect_type(manifest, dict, manifest_path, "manifest")

    unknown_keys = sorted(set(manifest.keys()) - TOP_LEVEL_KEYS)
    if unknown_keys:
        raise ManifestValidationError(
            f"unknown top-level keys: {', '.join(unknown_keys)}",
            manifest_path=manifest_path,
            field="manifest",
        )

    variables = manifest.get("variables")
    if variables is not None and not isinstance(variables, dict):
        raise ManifestValidationError(
            "variables must be an object",
            manifest_path=manifest_path,
            field="variables",
        )

    subdirs = manifest.get("subdirs")
    if subdirs is not None:
        _expect_type(subdirs, list, manifest_path, "subdirs")
        for index, subdir in enumerate(subdirs):
            _expect_type(subdir, str, manifest_path, f"subdirs[{index}]")

    dependencies = manifest.get("dependencies")
    if dependencies is not None:
        _expect_type(dependencies, list, manifest_path, "dependencies")
        names = set()
        for index, dependency in enumerate(dependencies):
            validate_dependency(dependency, manifest_path, f"dependencies[{index}]")
            dep_name = dependency["name"]
            if dep_name in names:
                raise ManifestValidationError(
                    f"duplicate dependency name '{dep_name}'",
                    manifest_path=manifest_path,
                    field=f"dependencies[{index}].name",
                )
            names.add(dep_name)

    for key in ("commands", "rootCommands", "postCommands", "postRootCommands"):
        commands = manifest.get(key)
        if commands is None:
            continue
        _expect_type(commands, list, manifest_path, key)
        for index, command in enumerate(commands):
            validate_command(command, manifest_path, f"{key}[{index}]")
