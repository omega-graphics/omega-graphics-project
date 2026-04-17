#!/usr/bin/env python3
import argparse
import filecmp
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class AssetCTestConfig:
    def __init__(
        self,
        omega_assetc: Path,
        assetbundle_verifier: Path,
        suite_dir: Path,
        asset_types: Path,
        verbose: bool,
    ) -> None:
        self.omega_assetc = omega_assetc
        self.assetbundle_verifier = assetbundle_verifier
        self.suite_dir = suite_dir
        self.asset_types = asset_types
        self.verbose = verbose


CONFIG: AssetCTestConfig | None = None


class AssetCIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if CONFIG is None:
            raise RuntimeError("CONFIG must be initialized before tests run")

        cls.cfg = CONFIG
        cls.manifest_file = (cls.cfg.suite_dir / "DemoAssets.manifest").resolve()
        cls.dist_dir = (cls.cfg.suite_dir / "dist").resolve()
        cls.v2_bundle = cls.dist_dir / "DemoAssetsV2.pak"
        cls.v2_key_file = cls.dist_dir / "DemoAssetsV2.pak.key"

        required_paths = [
            cls.cfg.omega_assetc,
            cls.cfg.assetbundle_verifier,
            cls.cfg.asset_types,
            cls.manifest_file,
            cls.v2_bundle,
            cls.v2_key_file,
        ]
        for path in required_paths:
            if not path.exists():
                raise RuntimeError(f"Required test path not found: {path}")

    def setUp(self) -> None:
        self._tempdir = tempfile.TemporaryDirectory(prefix="assetc-it-")
        self.work_dir = Path(self._tempdir.name)

    def tearDown(self) -> None:
        self._tempdir.cleanup()

    def _run(self, args: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
        proc = subprocess.run(
            args,
            text=True,
            capture_output=True,
            check=False,
            cwd=cwd or self.cfg.suite_dir,
        )
        if self.cfg.verbose:
            print("\n[CMD]", " ".join(str(arg) for arg in args))
            print("[RC]", proc.returncode)
            print(proc.stdout)
            print(proc.stderr)
        return proc

    def _run_assetc(self, output_path: Path) -> subprocess.CompletedProcess[str]:
        args = [
            str(self.cfg.omega_assetc),
            "--asset-types",
            str(self.cfg.asset_types),
            "--manifest",
            str(self.manifest_file),
            "--strip-prefix",
            "DemoAssets",
            "--type",
            "Materials/Hero.asset=material",
            "--key-file",
            str(self.v2_key_file),
            "--sign",
            "--output",
            str(output_path),
        ]
        return self._run(args, cwd=self.cfg.suite_dir)

    def _run_assetc_with_default_key(self, output_path: Path) -> subprocess.CompletedProcess[str]:
        args = [
            str(self.cfg.omega_assetc),
            "--asset-types",
            str(self.cfg.asset_types),
            "--manifest",
            str(self.manifest_file),
            "--strip-prefix",
            "DemoAssets",
            "--type",
            "Materials/Hero.asset=material",
            "--sign",
            "--output",
            str(output_path),
        ]
        return self._run(args, cwd=self.cfg.suite_dir)

    def _run_verifier(self, bundle_path: Path) -> subprocess.CompletedProcess[str]:
        return self._run(
            [str(self.cfg.assetbundle_verifier), str(bundle_path)],
            cwd=self.cfg.suite_dir,
        )

    def test_checked_in_v2_bundle_verifies(self) -> None:
        proc = self._run_verifier(self.v2_bundle)
        self.assertEqual(
            proc.returncode,
            0,
            msg=f"Verifier failed for checked-in v2 bundle\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}",
        )

    def test_assetc_generates_expected_v2_bundle(self) -> None:
        output_path = self.work_dir / "DemoAssetsV2.pak"
        proc = self._run_assetc(output_path)
        self.assertEqual(
            proc.returncode,
            0,
            msg=f"omega-assetc failed for v2 bundle\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}",
        )
        self.assertTrue(output_path.exists(), f"Expected v2 bundle was not created: {output_path}")
        self.assertTrue(
            filecmp.cmp(output_path, self.v2_bundle, shallow=False),
            "Generated v2 bundle does not match the checked-in golden bundle.",
        )
        shutil.copyfile(self.v2_key_file, Path(str(output_path) + ".key"))

        verify_proc = self._run_verifier(output_path)
        self.assertEqual(
            verify_proc.returncode,
            0,
            msg=(
                "Verifier failed for generated v2 bundle\n"
                f"STDOUT:\n{verify_proc.stdout}\nSTDERR:\n{verify_proc.stderr}"
            ),
        )

    def test_signed_bundle_rejects_tampered_payload(self) -> None:
        tampered_bundle = self.work_dir / "TamperedDemoAssetsV2.pak"
        shutil.copyfile(self.v2_bundle, tampered_bundle)
        shutil.copyfile(self.v2_key_file, Path(str(tampered_bundle) + ".key"))

        payload = bytearray(tampered_bundle.read_bytes())
        self.assertGreater(len(payload), 0, "Golden v2 bundle should not be empty.")
        payload[-1] ^= 0x01
        tampered_bundle.write_bytes(payload)

        proc = self._run_verifier(tampered_bundle)
        self.assertNotEqual(proc.returncode, 0, "Tampered bundle unexpectedly verified.")
        self.assertIn("tampered", proc.stderr.lower())

    def test_encrypted_bundle_requires_companion_key(self) -> None:
        encrypted_bundle = self.work_dir / "MissingKeyDemoAssetsV2.pak"
        shutil.copyfile(self.v2_bundle, encrypted_bundle)

        proc = self._run_verifier(encrypted_bundle)
        self.assertNotEqual(proc.returncode, 0, "Encrypted bundle unexpectedly opened without a key.")
        self.assertIn("requires a 32-byte key", proc.stderr.lower())

    def test_assetc_generates_companion_key_by_default(self) -> None:
        output_path = self.work_dir / "DefaultKeyDemoAssetsV2.pak"
        proc = self._run_assetc_with_default_key(output_path)
        self.assertEqual(
            proc.returncode,
            0,
            msg=f"omega-assetc failed while generating the default companion key\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}",
        )

        companion_key = Path(str(output_path) + ".key")
        self.assertTrue(output_path.exists(), f"Expected encrypted bundle was not created: {output_path}")
        self.assertTrue(companion_key.exists(), f"Expected companion key was not created: {companion_key}")

        verify_proc = self._run_verifier(output_path)
        self.assertEqual(
            verify_proc.returncode,
            0,
            msg=(
                "Verifier failed for bundle that used the default companion key\n"
                f"STDOUT:\n{verify_proc.stdout}\nSTDERR:\n{verify_proc.stderr}"
            ),
        )

    def test_assetbundle_rejects_legacy_like_bundle(self) -> None:
        legacy_like_bundle = self.work_dir / "LegacyLike.pak"
        legacy_like_bundle.write_bytes((1).to_bytes(4, byteorder="little", signed=False))

        proc = self._run_verifier(legacy_like_bundle)
        self.assertNotEqual(proc.returncode, 0, "Legacy-like bundle unexpectedly opened.")
        self.assertIn("unsupported legacy asset bundle format", proc.stderr.lower())


def main() -> int:
    parser = argparse.ArgumentParser(description="Run omega-assetc integration tests.")
    parser.add_argument("--omega-assetc", required=True, type=Path)
    parser.add_argument("--assetbundle-verifier", required=True, type=Path)
    parser.add_argument("--suite-dir", required=True, type=Path)
    parser.add_argument("--asset-types", required=True, type=Path)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    global CONFIG
    CONFIG = AssetCTestConfig(
        omega_assetc=args.omega_assetc.resolve(),
        assetbundle_verifier=args.assetbundle_verifier.resolve(),
        suite_dir=args.suite_dir.resolve(),
        asset_types=args.asset_types.resolve(),
        verbose=args.verbose,
    )

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(AssetCIntegrationTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
