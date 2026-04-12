from __future__ import annotations

import contextlib
import hashlib
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from autom_deps.cli import parse_args
from autom_deps.errors import DependencyExecutionError
from autom_deps.resolution import resolve_version_source
from autom_deps.runner import run


class FakeDownloadResponse:
    def __init__(self, payload: bytes, url: str) -> None:
        self._payload = payload
        self.url = url

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        return None

    def raise_for_status(self) -> None:
        return None

    def iter_content(self, chunk_size: int = 0):
        del chunk_size
        yield self._payload


class FakeJsonResponse:
    def __init__(self, payload) -> None:
        self._payload = payload

    def raise_for_status(self) -> None:
        return None

    def json(self):
        return self._payload


class AutomDepsPhase3Tests(unittest.TestCase):
    def setUp(self) -> None:
        self._tempdir = tempfile.TemporaryDirectory(prefix="autom-deps-phase3-")
        self.root = Path(self._tempdir.name)
        self.payload = b"phase3-payload"
        self.sha256 = hashlib.sha256(self.payload).hexdigest()

    def tearDown(self) -> None:
        self._tempdir.cleanup()

    @contextlib.contextmanager
    def _pushd(self, path: Path):
        prior = Path.cwd()
        os.chdir(path)
        try:
            yield
        finally:
            os.chdir(prior)

    def _write_manifest(self, payload: dict) -> None:
        (self.root / "AUTOMDEPS").write_text(json.dumps(payload, indent=2), encoding="utf-8")

    def _load_state(self) -> dict:
        return json.loads((self.root / ".automdeps" / "state.json").read_text(encoding="utf-8"))

    def _load_exports(self) -> dict:
        return json.loads((self.root / ".automdeps" / "exports.json").read_text(encoding="utf-8"))

    def _file_manifest(self) -> dict:
        return {
            "dependencies": [
                {
                    "name": "payload",
                    "type": "file",
                    "url": "https://example.invalid/payload.bin",
                    "dest": "./deps/payload.bin",
                    "sha256": self.sha256,
                    "exports": {
                        "bin": "./deps/payload.bin",
                    },
                }
            ]
        }

    def test_file_dependency_download_and_skip_with_checksum(self) -> None:
        self._write_manifest(self._file_manifest())

        with self._pushd(self.root), mock.patch(
            "autom_deps.dependencies.requests.get",
            return_value=FakeDownloadResponse(self.payload, "https://cdn.example.invalid/payload.bin"),
        ) as download_get:
            self.assertEqual(run([], parse_args([])), 0)
            self.assertEqual(download_get.call_count, 1)

        artifact = self.root / "deps" / "payload.bin"
        self.assertEqual(artifact.read_bytes(), self.payload)
        state = self._load_state()
        self.assertEqual(state["dependencies"]["payload"]["verification"]["sha256"], self.sha256)
        self.assertEqual(self._load_exports()["payload.bin"], str(artifact.resolve()))

        with self._pushd(self.root), mock.patch(
            "autom_deps.dependencies.requests.get",
            side_effect=AssertionError("skip path should not redownload"),
        ):
            self.assertEqual(run([], parse_args([])), 0)

        state = self._load_state()
        self.assertEqual(state["dependencies"]["payload"]["status"], "present")

    def test_verify_detects_checksum_mismatch(self) -> None:
        self._write_manifest(self._file_manifest())

        with self._pushd(self.root), mock.patch(
            "autom_deps.dependencies.requests.get",
            return_value=FakeDownloadResponse(self.payload, "https://cdn.example.invalid/payload.bin"),
        ):
            self.assertEqual(run([], parse_args([])), 0)

        artifact = self.root / "deps" / "payload.bin"
        artifact.write_bytes(b"corrupted")

        with self._pushd(self.root):
            with self.assertRaises(DependencyExecutionError):
                run([], parse_args(["--verify"]))

    def test_clean_removes_dependency_outputs_and_state(self) -> None:
        self._write_manifest(self._file_manifest())

        with self._pushd(self.root), mock.patch(
            "autom_deps.dependencies.requests.get",
            return_value=FakeDownloadResponse(self.payload, "https://cdn.example.invalid/payload.bin"),
        ):
            self.assertEqual(run([], parse_args([])), 0)

        artifact = self.root / "deps" / "payload.bin"
        self.assertTrue(artifact.exists())

        with self._pushd(self.root):
            self.assertEqual(run([], parse_args(["--clean", "payload"])), 0)

        self.assertFalse(artifact.exists())
        state = self._load_state()
        self.assertNotIn("payload", state["dependencies"])
        self.assertEqual(self._load_exports(), {})

    def test_dry_run_does_not_write_outputs_or_state(self) -> None:
        self._write_manifest(self._file_manifest())

        with self._pushd(self.root), mock.patch(
            "autom_deps.dependencies.requests.get",
            side_effect=AssertionError("dry-run should not download"),
        ):
            self.assertEqual(run([], parse_args(["--dry-run"])), 0)

        self.assertFalse((self.root / "deps" / "payload.bin").exists())
        self.assertFalse((self.root / ".automdeps" / "state.json").exists())

    def test_github_release_resolution_selects_latest_stable_asset(self) -> None:
        dependency = {
            "name": "pkg",
            "type": "archive",
            "url": "https://example.invalid/pkg.zip",
        }
        version_source = {
            "channel": "stable",
            "strategy": "github-releases",
            "repo": "omega-graphics/example",
            "asset_template": "pkg-{version}.zip",
        }
        releases = [
            {
                "tag_name": "v9.0.0-rc1",
                "draft": False,
                "prerelease": True,
                "assets": [],
            },
            {
                "tag_name": "v8.2.1",
                "draft": False,
                "prerelease": False,
                "assets": [
                    {
                        "name": "pkg-8.2.1.zip",
                        "browser_download_url": "https://downloads.example.invalid/pkg-8.2.1.zip",
                    }
                ],
            },
        ]

        with mock.patch("autom_deps.resolution.requests.get", return_value=FakeJsonResponse(releases)):
            resolved = resolve_version_source(version_source, dependency)

        self.assertEqual(resolved["version"], "8.2.1")
        self.assertEqual(resolved["url"], "https://downloads.example.invalid/pkg-8.2.1.zip")

    def test_legacy_git_clone_skips_existing_checkout(self) -> None:
        repo_dir = self.root / "deps" / "rapidjson"
        (repo_dir / ".git").mkdir(parents=True)
        self._write_manifest(
            {
                "commands": [
                    {
                        "type": "git_clone",
                        "url": "https://example.invalid/rapidjson.git",
                        "dest": "./deps/rapidjson",
                        "branch": "default",
                    }
                ]
            }
        )

        with self._pushd(self.root), mock.patch(
            "autom_deps.runner._run_shell",
            side_effect=AssertionError("existing checkout should not reclone"),
        ):
            self.assertEqual(run([], parse_args([])), 0)


if __name__ == "__main__":
    unittest.main()
