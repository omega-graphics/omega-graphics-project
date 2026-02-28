#!/usr/bin/env python3
import argparse
import re
import subprocess
import tempfile
import unittest
from pathlib import Path


class WrapGenTestConfig:
    def __init__(self, omega_wrapgen: Path, suite_dir: Path, verbose: bool) -> None:
        self.omega_wrapgen = omega_wrapgen
        self.suite_dir = suite_dir
        self.verbose = verbose


CONFIG: WrapGenTestConfig | None = None


class WrapGenIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if CONFIG is None:
            raise RuntimeError("CONFIG must be initialized before tests run")
        cls.cfg = CONFIG
        if not cls.cfg.omega_wrapgen.exists():
            raise RuntimeError(f"omega-wrapgen binary not found: {cls.cfg.omega_wrapgen}")

        cls.input_file = (cls.cfg.suite_dir / "example.owrap").resolve()
        if not cls.input_file.exists():
            raise RuntimeError(f"Test input file not found: {cls.input_file}")
        cls.builtin_types_file = (cls.cfg.suite_dir / "builtin-types.owrap").resolve()
        if not cls.builtin_types_file.exists():
            raise RuntimeError(f"Builtin types test input file not found: {cls.builtin_types_file}")
        cls.namespace_file = (cls.cfg.suite_dir / "namespace.owrap").resolve()
        if not cls.namespace_file.exists():
            raise RuntimeError(f"Namespace test input file not found: {cls.namespace_file}")
        cls.interface_file = (cls.cfg.suite_dir / "interface.owrap").resolve()
        if not cls.interface_file.exists():
            raise RuntimeError(f"Interface test input file not found: {cls.interface_file}")
        cls.structs_file = (cls.cfg.suite_dir / "structs.owrap").resolve()
        if not cls.structs_file.exists():
            raise RuntimeError(f"Structs test input file not found: {cls.structs_file}")
        cls.string_array_file = (cls.cfg.suite_dir / "string-array.owrap").resolve()
        if not cls.string_array_file.exists():
            raise RuntimeError(f"String array test input file not found: {cls.string_array_file}")
        cls.class_fields_file = (cls.cfg.suite_dir / "class-fields.owrap").resolve()
        if not cls.class_fields_file.exists():
            raise RuntimeError(f"Class fields test input file not found: {cls.class_fields_file}")
        cls.semantic_unknown_type_file = (cls.cfg.suite_dir / "semantic-unknown-type.owrap").resolve()
        if not cls.semantic_unknown_type_file.exists():
            raise RuntimeError(f"Semantic unknown type test input file not found: {cls.semantic_unknown_type_file}")
        cls.semantic_invalid_void_file = (cls.cfg.suite_dir / "semantic-invalid-void.owrap").resolve()
        if not cls.semantic_invalid_void_file.exists():
            raise RuntimeError(f"Semantic invalid void test input file not found: {cls.semantic_invalid_void_file}")

    def setUp(self) -> None:
        self._tempdir = tempfile.TemporaryDirectory(prefix="wrapgen-it-")
        self.work_dir = Path(self._tempdir.name)

    def tearDown(self) -> None:
        self._tempdir.cleanup()

    def _run_wrapgen(self, args: list[str]) -> subprocess.CompletedProcess[str]:
        cmd = [str(self.cfg.omega_wrapgen), *args]
        proc = subprocess.run(
            cmd,
            text=True,
            capture_output=True,
            check=False,
            cwd=self.cfg.suite_dir,
        )
        if self.cfg.verbose:
            print("\n[WRAPGEN CMD]", " ".join(cmd))
            print("[WRAPGEN RC]", proc.returncode)
            print(proc.stdout)
            print(proc.stderr)
        return proc

    def _assert_mode_generates(self, mode_flag: str, expected_files: list[str]) -> None:
        out_dir = self.work_dir / mode_flag.lstrip("-")
        out_dir.mkdir(parents=True, exist_ok=True)

        proc = self._run_wrapgen([mode_flag, "-o", str(out_dir), str(self.input_file)])
        self.assertEqual(
            proc.returncode,
            0,
            msg=(
                f"omega-wrapgen failed for mode {mode_flag}\n"
                f"STDOUT:\n{proc.stdout}\n"
                f"STDERR:\n{proc.stderr}"
            ),
        )

        for filename in expected_files:
            path = out_dir / filename
            self.assertTrue(path.exists(), f"Expected generated file does not exist: {path}")
            content = path.read_text(encoding="utf-8")
            self.assertTrue(content.strip(), f"Generated file is empty: {path}")

    def test_all_language_modes_generate_outputs(self) -> None:
        cases = [
            ("--cc", ["example.h", "example.cpp"]),
            ("--python", ["example.py"]),
            ("--go", ["example.go"]),
            ("--java", ["example.java"]),
            ("--swift", ["example.swift"]),
            ("--rust", ["example.rs"]),
        ]
        for mode_flag, expected in cases:
            with self.subTest(mode=mode_flag):
                self._assert_mode_generates(mode_flag, expected)

    def test_unknown_mode_fails(self) -> None:
        proc = self._run_wrapgen(["--bogus", str(self.input_file)])
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("unknown option", proc.stderr.lower())

    def test_builtin_scalar_types_parse_and_generate_c(self) -> None:
        out_dir = self.work_dir / "builtin-types"
        out_dir.mkdir(parents=True, exist_ok=True)

        proc = self._run_wrapgen(["--cc", "-o", str(out_dir), str(self.builtin_types_file)])
        self.assertEqual(
            proc.returncode,
            0,
            msg=(
                "omega-wrapgen failed for builtin types fixture\n"
                f"STDOUT:\n{proc.stdout}\n"
                f"STDERR:\n{proc.stderr}"
            ),
        )

        header = out_dir / "builtin-types.h"
        source = out_dir / "builtin-types.cpp"
        self.assertTrue(header.exists(), f"Expected generated file does not exist: {header}")
        self.assertTrue(source.exists(), f"Expected generated file does not exist: {source}")

        combined = header.read_text(encoding="utf-8") + "\n" + source.read_text(encoding="utf-8")
        for builtin in ("float", "long", "double"):
            self.assertIn(builtin, combined, f"Generated C wrapper output missing builtin type: {builtin}")

    def test_c_generation_emits_expected_class_wrappers(self) -> None:
        out_dir = self.work_dir / "c-wrapper-shape"
        out_dir.mkdir(parents=True, exist_ok=True)

        proc = self._run_wrapgen(["--cc", "-o", str(out_dir), str(self.input_file)])
        self.assertEqual(
            proc.returncode,
            0,
            msg=(
                "omega-wrapgen failed for C wrapper shape fixture\n"
                f"STDOUT:\n{proc.stdout}\n"
                f"STDERR:\n{proc.stderr}"
            ),
        )

        header = out_dir / "example.h"
        source = out_dir / "example.cpp"
        self.assertTrue(header.exists(), f"Expected generated file does not exist: {header}")
        self.assertTrue(source.exists(), f"Expected generated file does not exist: {source}")

        header_content = header.read_text(encoding="utf-8")
        source_content = source.read_text(encoding="utf-8")

        # Class handle + instance method wrappers should be emitted for C mode.
        self.assertIn("typedef struct __TestClass * TestClass;", header_content)
        self.assertIn("TestClass__testFunc(", header_content)
        self.assertIn("TestClass__otherFunc(", header_content)
        self.assertIn("__TestClass* __self", header_content)

        # Source should bridge calls into the wrapped C++ instance object.
        self.assertIn("extern \"C\" void TestClass__testFunc(", source_content)
        self.assertIn("extern \"C\" void TestClass__otherFunc(", source_content)
        self.assertIn("__self->obj.testFunc(", source_content)
        self.assertIn("__self->obj.otherFunc(", source_content)

    def test_c_generation_processes_namespace_decls(self) -> None:
        out_dir = self.work_dir / "c-namespace-shape"
        out_dir.mkdir(parents=True, exist_ok=True)

        proc = self._run_wrapgen(["--cc", "-o", str(out_dir), str(self.namespace_file)])
        self.assertEqual(
            proc.returncode,
            0,
            msg=(
                "omega-wrapgen failed for namespace fixture\n"
                f"STDOUT:\n{proc.stdout}\n"
                f"STDERR:\n{proc.stderr}"
            ),
        )

        header = out_dir / "namespace.h"
        source = out_dir / "namespace.cpp"
        self.assertTrue(header.exists(), f"Expected generated file does not exist: {header}")
        self.assertTrue(source.exists(), f"Expected generated file does not exist: {source}")

        header_content = header.read_text(encoding="utf-8")
        source_content = source.read_text(encoding="utf-8")

        # Namespace free-function wrapper and namespace class handle should be emitted.
        self.assertIn("int Math__add(", header_content)
        self.assertIn("typedef struct Math__Vec * Math__Vec;", header_content)
        self.assertIn("MathVec__length(", header_content)

        # C++ calls should be namespace-qualified in the generated source.
        self.assertIn("return Math::add(", source_content)
        self.assertIn("struct Math__Vec{Math::Vec obj;};", source_content)
        self.assertIn("__self->obj.length(", source_content)

    def test_c_generation_processes_interface_decls(self) -> None:
        out_dir = self.work_dir / "c-interface-shape"
        out_dir.mkdir(parents=True, exist_ok=True)

        proc = self._run_wrapgen(["--cc", "-o", str(out_dir), str(self.interface_file)])
        self.assertEqual(
            proc.returncode,
            0,
            msg=(
                "omega-wrapgen failed for interface fixture\n"
                f"STDOUT:\n{proc.stdout}\n"
                f"STDERR:\n{proc.stderr}"
            ),
        )

        header = out_dir / "interface.h"
        source = out_dir / "interface.cpp"
        self.assertTrue(header.exists(), f"Expected generated file does not exist: {header}")
        self.assertTrue(source.exists(), f"Expected generated file does not exist: {source}")

        header_content = header.read_text(encoding="utf-8")
        source_content = source.read_text(encoding="utf-8")

        self.assertIn("typedef struct __Drawable Drawable;", header_content)
        self.assertIn("typedef struct __DrawableVTable __DrawableVTable;", header_content)
        self.assertIn("struct __Drawable{void *self; const __DrawableVTable *vtable;};", header_content)
        self.assertIn("void (*draw)(void *self", header_content)
        self.assertIn("double (*area)(void *self", header_content)
        self.assertIn("Drawable__draw(Drawable iface", header_content)
        self.assertIn("Drawable__area(Drawable iface", header_content)

        self.assertIn("iface.vtable->draw(iface.self", source_content)
        self.assertIn("return iface.vtable->area(iface.self", source_content)

    def test_c_generation_processes_struct_decls(self) -> None:
        out_dir = self.work_dir / "c-struct-shape"
        out_dir.mkdir(parents=True, exist_ok=True)

        proc = self._run_wrapgen(["--cc", "-o", str(out_dir), str(self.structs_file)])
        self.assertEqual(
            proc.returncode,
            0,
            msg=(
                "omega-wrapgen failed for structs fixture\n"
                f"STDOUT:\n{proc.stdout}\n"
                f"STDERR:\n{proc.stderr}"
            ),
        )

        header = out_dir / "structs.h"
        source = out_dir / "structs.cpp"
        self.assertTrue(header.exists(), f"Expected generated file does not exist: {header}")
        self.assertTrue(source.exists(), f"Expected generated file does not exist: {source}")

        header_content = header.read_text(encoding="utf-8")
        source_content = source.read_text(encoding="utf-8")

        # Top-level struct translation.
        self.assertIn("typedef struct __Pixel Pixel;", header_content)
        self.assertIn("struct __Pixel{", header_content)
        self.assertIn("int r;", header_content)
        self.assertIn("int g;", header_content)
        self.assertIn("int b;", header_content)
        self.assertIn("float alpha;", header_content)

        # Namespaced struct translation.
        self.assertIn("typedef struct Geometry__Bounds Geometry__Bounds;", header_content)
        self.assertIn("struct Geometry__Bounds{", header_content)
        self.assertIn("double width;", header_content)
        self.assertIn("double height;", header_content)
        self.assertIn("long area;", header_content)

        # Structs should be header-only C ABI declarations.
        self.assertNotIn("Geometry__Bounds(", source_content)
        self.assertNotIn("__Pixel(", source_content)

    def test_c_generation_processes_string_and_array_types(self) -> None:
        out_dir = self.work_dir / "c-string-array-shape"
        out_dir.mkdir(parents=True, exist_ok=True)

        proc = self._run_wrapgen(["--cc", "-o", str(out_dir), str(self.string_array_file)])
        self.assertEqual(
            proc.returncode,
            0,
            msg=(
                "omega-wrapgen failed for string/array fixture\n"
                f"STDOUT:\n{proc.stdout}\n"
                f"STDERR:\n{proc.stderr}"
            ),
        )

        header = out_dir / "string-array.h"
        source = out_dir / "string-array.cpp"
        self.assertTrue(header.exists(), f"Expected generated file does not exist: {header}")
        self.assertTrue(source.exists(), f"Expected generated file does not exist: {source}")

        header_content = header.read_text(encoding="utf-8")
        source_content = source.read_text(encoding="utf-8")

        self.assertIn("const char * title;", header_content)

        string_array_alias = re.search(
            r"typedef struct (OmegaArray_[A-Za-z0-9_]+)\{const char \* \*data; long len;\} \1;",
            header_content,
        )
        self.assertIsNotNone(string_array_alias, "Expected generated array typedef for string[]")
        string_array_alias_name = string_array_alias.group(1)

        int_array_alias = re.search(
            r"typedef struct (OmegaArray_[A-Za-z0-9_]+)\{int \*data; long len;\} \1;",
            header_content,
        )
        self.assertIsNotNone(int_array_alias, "Expected generated array typedef for int[]")
        int_array_alias_name = int_array_alias.group(1)

        self.assertIn(f"{string_array_alias_name} lines;", header_content)
        self.assertIn(f"{int_array_alias_name} scores;", header_content)
        self.assertIn(f"{string_array_alias_name} TextService__getLines(", header_content)
        self.assertIn(f"{int_array_alias_name} TextService__getScores(", header_content)
        self.assertIn(f"{string_array_alias_name} lines", header_content)
        self.assertIn(f"{int_array_alias_name} scores", header_content)
        text_batch_struct = re.search(r"struct __TextBatch\{(?P<body>.*?)\};", header_content, re.DOTALL)
        self.assertIsNotNone(text_batch_struct, "Expected TextBatch struct declaration")
        self.assertNotIn(
            "typedef struct OmegaArray_",
            text_batch_struct.group("body"),
            "Array aliases should not be emitted inside struct bodies",
        )

        self.assertIn("__self->obj.setLines(lines);", source_content)
        self.assertIn("__self->obj.setScores(scores);", source_content)
        self.assertIn("return __self->obj.getLines();", source_content)
        self.assertIn("return __self->obj.getScores();", source_content)

    def test_c_generation_processes_class_fields(self) -> None:
        out_dir = self.work_dir / "c-class-fields-shape"
        out_dir.mkdir(parents=True, exist_ok=True)

        proc = self._run_wrapgen(["--cc", "-o", str(out_dir), str(self.class_fields_file)])
        self.assertEqual(
            proc.returncode,
            0,
            msg=(
                "omega-wrapgen failed for class fields fixture\n"
                f"STDOUT:\n{proc.stdout}\n"
                f"STDERR:\n{proc.stderr}"
            ),
        )

        header = out_dir / "class-fields.h"
        source = out_dir / "class-fields.cpp"
        self.assertTrue(header.exists(), f"Expected generated file does not exist: {header}")
        self.assertTrue(source.exists(), f"Expected generated file does not exist: {source}")

        header_content = header.read_text(encoding="utf-8")
        source_content = source.read_text(encoding="utf-8")

        self.assertIn("typedef struct __Config * Config;", header_content)
        self.assertIn("const char * Config__get_name(__Config* __self);", header_content)
        self.assertIn("void Config__set_name(__Config* __self,const char * value);", header_content)
        self.assertIn("int Config__get_version(__Config* __self);", header_content)
        self.assertIn("void Config__set_version(__Config* __self,int value);", header_content)
        self.assertIn("int Config__get_id(__Config* __self);", header_content)
        self.assertNotIn("Config__set_id(", header_content)

        self.assertIn("return __self->obj.name;", source_content)
        self.assertIn("__self->obj.name = value;", source_content)
        self.assertIn("return __self->obj.version;", source_content)
        self.assertIn("__self->obj.version = value;", source_content)
        self.assertIn("return __self->obj.id;", source_content)

    def test_semantic_unknown_type_reports_diagnostic(self) -> None:
        out_dir = self.work_dir / "semantic-unknown-type"
        out_dir.mkdir(parents=True, exist_ok=True)

        proc = self._run_wrapgen(["--cc", "-o", str(out_dir), str(self.semantic_unknown_type_file)])
        self.assertNotEqual(proc.returncode, 0, "Expected semantic unknown-type fixture to fail")
        combined = proc.stdout + "\n" + proc.stderr
        self.assertIn("SEMANTIC ERROR", combined)
        self.assertIn("Unknown type 'Vec3'", combined)

    def test_semantic_invalid_void_usage_reports_diagnostic(self) -> None:
        out_dir = self.work_dir / "semantic-invalid-void"
        out_dir.mkdir(parents=True, exist_ok=True)

        proc = self._run_wrapgen(["--cc", "-o", str(out_dir), str(self.semantic_invalid_void_file)])
        self.assertNotEqual(proc.returncode, 0, "Expected semantic invalid-void fixture to fail")
        combined = proc.stdout + "\n" + proc.stderr
        self.assertIn("SEMANTIC ERROR", combined)
        self.assertIn("Invalid type 'void'", combined)



def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run omega-wrapgen integration tests")
    parser.add_argument("--omega-wrapgen", required=True, type=Path)
    parser.add_argument("--suite-dir", required=True, type=Path)
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def main() -> int:
    global CONFIG
    args = parse_args()
    CONFIG = WrapGenTestConfig(
        omega_wrapgen=args.omega_wrapgen.resolve(),
        suite_dir=args.suite_dir.resolve(),
        verbose=args.verbose,
    )

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(WrapGenIntegrationTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
