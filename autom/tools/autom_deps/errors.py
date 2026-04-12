from __future__ import annotations

from dataclasses import dataclass


@dataclass
class AutomDepsError(Exception):
    message: str

    def __str__(self) -> str:
        return self.message


@dataclass
class ManifestValidationError(AutomDepsError):
    manifest_path: str | None = None
    field: str | None = None

    def __str__(self) -> str:
        parts = []
        if self.manifest_path:
            parts.append(self.manifest_path)
        if self.field:
            parts.append(self.field)
        if parts:
            return f"{' :: '.join(parts)} :: {self.message}"
        return self.message


@dataclass
class CommandExecutionError(AutomDepsError):
    manifest_path: str | None = None
    command_type: str | None = None
    command_index: int | None = None

    def __str__(self) -> str:
        parts = []
        if self.manifest_path:
            parts.append(self.manifest_path)
        if self.command_type:
            parts.append(self.command_type)
        if self.command_index is not None:
            parts.append(f"command[{self.command_index}]")
        if parts:
            return f"{' :: '.join(parts)} :: {self.message}"
        return self.message


@dataclass
class DependencyExecutionError(AutomDepsError):
    manifest_path: str | None = None
    dependency_name: str | None = None
    dependency_type: str | None = None

    def __str__(self) -> str:
        parts = []
        if self.manifest_path:
            parts.append(self.manifest_path)
        if self.dependency_name:
            parts.append(self.dependency_name)
        if self.dependency_type:
            parts.append(self.dependency_type)
        if parts:
            return f"{' :: '.join(parts)} :: {self.message}"
        return self.message
