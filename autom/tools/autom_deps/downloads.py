from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

import requests

from .ui import StatusPrinter, format_bytes


@dataclass
class DownloadResult:
    final_url: str
    bytes_written: int
    resumed: bool


class DownloadManager:
    def __init__(
        self,
        *,
        session: requests.Session | None = None,
        resume_enabled: bool = True,
        single_stream: bool = True,
    ) -> None:
        self.session = session or requests.Session()
        self.resume_enabled = resume_enabled
        self.single_stream = single_stream

    def download(
        self,
        url: str,
        dest: str | Path,
        *,
        printer: StatusPrinter,
        label: str,
        dry_run: bool = False,
    ) -> DownloadResult:
        dest_path = Path(dest)
        if dry_run:
            printer.note("DRYRUN", f"would download {url} -> {dest_path}")
            return DownloadResult(final_url=url, bytes_written=0, resumed=False)

        dest_path.parent.mkdir(parents=True, exist_ok=True)
        partial_path = Path(f"{dest_path}.part")
        resume_from = partial_path.stat().st_size if self.resume_enabled and partial_path.exists() else 0

        headers = {"User-Agent": "autom-deps/1.0"}
        if resume_from > 0:
            headers["Range"] = f"bytes={resume_from}-"

        with self.session.get(
            url,
            stream=True,
            allow_redirects=True,
            timeout=(30, 300),
            headers=headers,
        ) as response:
            if response.status_code == 416 and resume_from > 0:
                os.replace(partial_path, dest_path)
                printer.note("OK", f"reused completed partial download {dest_path}")
                return DownloadResult(final_url=str(response.url), bytes_written=resume_from, resumed=True)

            response.raise_for_status()

            resumed = resume_from > 0 and response.status_code == 206
            if not resumed:
                resume_from = 0

            total_bytes = _resolve_total_bytes(response, resume_from)
            chunk_size = _pick_chunk_size(total_bytes)
            mode = "ab" if resumed else "wb"

            with partial_path.open(mode) as stream:
                bytes_written = resume_from
                for chunk in response.iter_content(chunk_size=chunk_size):
                    if not chunk:
                        continue
                    stream.write(chunk)
                    bytes_written += len(chunk)
                    printer.progress(label, bytes_written, total_bytes)

            os.replace(partial_path, dest_path)
            printer.progress(label, bytes_written, total_bytes, force=True)
            if resumed:
                printer.note("OK", f"resumed download to {dest_path} ({format_bytes(bytes_written)})")
            else:
                printer.note("OK", f"downloaded {format_bytes(bytes_written)} from {response.url}")
            return DownloadResult(final_url=str(response.url), bytes_written=bytes_written, resumed=resumed)


def _resolve_total_bytes(response: requests.Response, resume_from: int) -> int | None:
    content_range = response.headers.get("Content-Range")
    if content_range and "/" in content_range:
        total_value = content_range.rsplit("/", 1)[-1]
        if total_value.isdigit():
            return int(total_value)

    content_length = response.headers.get("Content-Length")
    if content_length and content_length.isdigit():
        return int(content_length) + resume_from
    return None


def _pick_chunk_size(total_bytes: int | None) -> int:
    if total_bytes is None:
        return 1024 * 1024
    if total_bytes >= 256 * 1024 * 1024:
        return 4 * 1024 * 1024
    if total_bytes >= 64 * 1024 * 1024:
        return 2 * 1024 * 1024
    return 1024 * 1024
