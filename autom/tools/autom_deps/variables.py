from __future__ import annotations

import json
import re

from .errors import ManifestValidationError


VARIABLE_PATTERN = re.compile(r"\$\(([A-Za-z0-9_]+)\)")
VARIABLE_NAME_PATTERN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def validate_variable_name(name: str, manifest_path: str, field: str) -> None:
    if not VARIABLE_NAME_PATTERN.match(name):
        raise ManifestValidationError(
            f"invalid variable name '{name}'",
            manifest_path=manifest_path,
            field=field,
        )


def resolve_string(
    value: str,
    variables: dict[str, object],
    manifest_path: str,
    field: str,
    stack: list[str] | None = None,
) -> str:
    active_stack = [] if stack is None else list(stack)

    def replace(match: re.Match[str]) -> str:
        key = match.group(1)
        if key not in variables:
            raise ManifestValidationError(
                f"undefined variable '{key}'",
                manifest_path=manifest_path,
                field=field,
            )
        if key in active_stack:
            cycle = " -> ".join(active_stack + [key])
            raise ManifestValidationError(
                f"cyclic variable reference detected: {cycle}",
                manifest_path=manifest_path,
                field=field,
            )

        resolved_value = variables[key]
        if isinstance(resolved_value, str):
            return resolve_string(
                resolved_value,
                variables,
                manifest_path=manifest_path,
                field=f"variables.{key}",
                stack=active_stack + [key],
            )
        return json.dumps(resolved_value)

    return VARIABLE_PATTERN.sub(replace, value)


def resolve_value(
    value,
    variables: dict[str, object],
    manifest_path: str,
    field: str,
):
    if isinstance(value, str):
        return resolve_string(value, variables, manifest_path, field)
    if isinstance(value, list):
        return [
            resolve_value(item, variables, manifest_path, f"{field}[{index}]")
            for index, item in enumerate(value)
        ]
    if isinstance(value, dict):
        return {
            key: resolve_value(item, variables, manifest_path, f"{field}.{key}")
            for key, item in value.items()
        }
    return value


def validate_local_variables(
    local_variables: dict[str, object],
    inherited_variables: dict[str, object],
    manifest_path: str,
) -> None:
    combined = dict(inherited_variables)
    combined.update(local_variables)
    for key, value in local_variables.items():
        validate_variable_name(key, manifest_path, f"variables.{key}")
        if not isinstance(value, (str, int, float, bool, list, dict)) and value is not None:
            raise ManifestValidationError(
                f"unsupported variable value type '{type(value).__name__}'",
                manifest_path=manifest_path,
                field=f"variables.{key}",
            )
        if isinstance(value, str):
            resolve_string(value, combined, manifest_path, f"variables.{key}")
