from __future__ import annotations

import json
import sys
import time
from dataclasses import dataclass, field
from typing import TextIO


def format_bytes(value: int) -> str:
    if value < 1024:
        return f"{value} B"

    size = float(value)
    for unit in ("KiB", "MiB", "GiB", "TiB"):
        size /= 1024.0
        if size < 1024.0 or unit == "TiB":
            return f"{size:.1f} {unit}"
    return f"{value} B"


@dataclass
class StatusPrinter:
    total_steps: int
    verbose: bool = False
    quiet: bool = False
    json_log: bool = False
    show_progress: bool = False
    stream: TextIO = field(default_factory=lambda: sys.stdout)
    current_step: int = 0
    completed_steps: int = 0
    skipped_steps: int = 0
    failed_steps: int = 0
    _last_progress_at: dict[str, float] = field(default_factory=dict)

    def header(self, root_dir: str, target: str, mode: str) -> None:
        if self.json_log:
            self._emit_json("session_start", root=root_dir, target=target, mode=mode, total_steps=self.total_steps)
            return
        if self.quiet:
            return
        self.stream.write("autom-deps :: session\n")
        self.stream.write(f"root   : {root_dir}\n")
        self.stream.write(f"target : {target}\n")
        self.stream.write(f"mode   : {mode}\n")
        self.stream.write(f"steps  : {self.total_steps}\n")

    def step(self, kind: str, message: str) -> None:
        self.current_step += 1
        if self.current_step > self.total_steps:
            self.total_steps = self.current_step

        if self.json_log:
            self._emit_json(
                "step",
                step=self.current_step,
                total_steps=self.total_steps,
                kind=kind,
                message=message,
            )
            return

        if self.quiet:
            return

        self.stream.write(f"[{self.current_step}/{self.total_steps}] {kind:<8} {message}\n")

    def note(self, kind: str, message: str) -> None:
        if self.json_log:
            self._emit_json("note", kind=kind, message=message)
            return

        if self.quiet and kind not in {"WARN", "ERROR", "DONE"}:
            return
        if not self.verbose and kind == "INFO":
            return

        self.stream.write(f"[{kind}] {message}\n")

    def progress(self, label: str, downloaded: int, total: int | None = None, *, force: bool = False) -> None:
        if not self.show_progress:
            return

        now = time.monotonic()
        if not force and now - self._last_progress_at.get(label, 0.0) < 0.2:
            return
        self._last_progress_at[label] = now

        if total is not None and total > 0:
            percent = int((downloaded / total) * 100)
            message = f"{label} {format_bytes(downloaded)} / {format_bytes(total)} ({percent}%)"
        else:
            message = f"{label} {format_bytes(downloaded)}"

        if self.json_log:
            self._emit_json("progress", label=label, downloaded=downloaded, total=total)
            return

        if self.quiet:
            return

        self.stream.write(f"  progress : {message}\n")

    def record_completion(self, *, skipped: bool) -> None:
        if skipped:
            self.skipped_steps += 1
        else:
            self.completed_steps += 1

    def record_failure(self) -> None:
        self.failed_steps += 1

    def finish(self) -> None:
        if self.json_log:
            self._emit_json(
                "summary",
                completed=self.completed_steps,
                skipped=self.skipped_steps,
                failed=self.failed_steps,
                total_steps=self.total_steps,
            )
            return

        self.stream.write("Summary\n")
        self.stream.write(f"  done    : {self.completed_steps}\n")
        self.stream.write(f"  skipped : {self.skipped_steps}\n")
        self.stream.write(f"  failed  : {self.failed_steps}\n")

    def _emit_json(self, event: str, **payload: object) -> None:
        record = {"event": event, **payload}
        self.stream.write(json.dumps(record, sort_keys=True) + "\n")


def emit_error(message: str, *, json_log: bool, stream: TextIO | None = None) -> None:
    output = stream or sys.stderr
    if json_log:
        output.write(json.dumps({"event": "error", "message": message}, sort_keys=True) + "\n")
        return
    output.write(f"[ERROR] {message}\n")
