#!/usr/bin/env python3
import argparse
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class WrapGenTestConfig:
    def __init__(self, omega_wrapgen: Path, parse_test: Path, suite_dir: Path, verbose: bool) -> None:
        self.omega_wrapgen = omega_wrapgen
        self.parse_test = parse_test
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
        if not cls.cfg.parse_test.exists():
            raise RuntimeError(f"parse-test binary not found: {cls.cfg.parse_test}")

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
        cls.phase0_baseline_file = (cls.cfg.suite_dir / "phase0-baseline.owrap").resolve()
        if not cls.phase0_baseline_file.exists():
            raise RuntimeError(f"Phase 0 baseline fixture not found: {cls.phase0_baseline_file}")
        cls.phase0_baseline_ast_file = (cls.cfg.suite_dir / "phase0-baseline.ast").resolve()
        if not cls.phase0_baseline_ast_file.exists():
            raise RuntimeError(f"Phase 0 baseline AST golden file not found: {cls.phase0_baseline_ast_file}")
        cls.unsupported_alias_file = (cls.cfg.suite_dir / "unsupported-alias.owrap").resolve()
        if not cls.unsupported_alias_file.exists():
            raise RuntimeError(f"Unsupported alias fixture not found: {cls.unsupported_alias_file}")
        cls.quirk_optional_commas_file = (cls.cfg.suite_dir / "quirk-optional-commas.owrap").resolve()
        if not cls.quirk_optional_commas_file.exists():
            raise RuntimeError(f"Optional comma quirk fixture not found: {cls.quirk_optional_commas_file}")
        cls.syntax_missing_colon_field_file = (cls.cfg.suite_dir / "syntax-missing-colon-field.owrap").resolve()
        if not cls.syntax_missing_colon_field_file.exists():
            raise RuntimeError(f"Missing-colon syntax fixture not found: {cls.syntax_missing_colon_field_file}")
        cls.syntax_missing_lbrace_class_file = (cls.cfg.suite_dir / "syntax-missing-lbrace-class.owrap").resolve()
        if not cls.syntax_missing_lbrace_class_file.exists():
            raise RuntimeError(f"Missing-lbrace syntax fixture not found: {cls.syntax_missing_lbrace_class_file}")
        cls.syntax_missing_return_type_file = (cls.cfg.suite_dir / "syntax-missing-return-type.owrap").resolve()
        if not cls.syntax_missing_return_type_file.exists():
            raise RuntimeError(f"Missing-return-type syntax fixture not found: {cls.syntax_missing_return_type_file}")
        cls.syntax_interface_field_file = (cls.cfg.suite_dir / "syntax-interface-field.owrap").resolve()
        if not cls.syntax_interface_field_file.exists():
            raise RuntimeError(f"Interface-field syntax fixture not found: {cls.syntax_interface_field_file}")

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

    def _run_parse_test(self, input_file: Path) -> subprocess.CompletedProcess[str]:
        cmd = [str(self.cfg.parse_test), str(input_file)]
        proc = subprocess.run(
            cmd,
            text=True,
            capture_output=True,
            check=False,
            cwd=self.cfg.suite_dir,
        )
        if self.cfg.verbose:
            print("\n[PARSE-TEST CMD]", " ".join(cmd))
            print("[PARSE-TEST RC]", proc.returncode)
            print(proc.stdout)
            print(proc.stderr)
        return proc

    def _compile_generated_cpp(self, source_file: Path, output_file: Path) -> subprocess.CompletedProcess[str]:
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
        if compiler is None:
            self.skipTest("No C++ compiler found for generated C++ compile smoke test")
        cmd = [compiler, "-std=c++17", "-c", str(source_file), "-o", str(output_file)]
        proc = subprocess.run(cmd, text=True, capture_output=True, check=False)
        if self.cfg.verbose:
            print("\n[CXX CMD]", " ".join(cmd))
            print("[CXX RC]", proc.returncode)
            print(proc.stdout)
            print(proc.stderr)
        return proc

    def _assert_wrapgen_parse_fails(self, input_file: Path, expected_message: str) -> None:
        out_dir = self.work_dir / input_file.stem
        out_dir.mkdir(parents=True, exist_ok=True)

        proc = self._run_wrapgen(["--cc", "-o", str(out_dir), str(input_file)])
        self.assertNotEqual(proc.returncode, 0, f"Expected parser fixture to fail: {input_file}")
        combined = proc.stdout + "\n" + proc.stderr
        self.assertIn("PARSE ERROR", combined)
        self.assertIn(expected_message, combined)

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

    def test_phase0_baseline_parser_output_matches_golden(self) -> None:
        proc = self._run_parse_test(self.phase0_baseline_file)
        self.assertEqual(
            proc.returncode,
            0,
            msg=(
                "parse-test failed for Phase 0 baseline fixture\n"
                f"STDOUT:\n{proc.stdout}\n"
                f"STDERR:\n{proc.stderr}"
            ),
        )

        expected = self.phase0_baseline_ast_file.read_text(encoding="utf-8")
        self.assertEqual(expected, proc.stdout)

    def test_phase0_baseline_supported_surface_generates_c_output(self) -> None:
        out_dir = self.work_dir / "phase0-baseline-shape"
        out_dir.mkdir(parents=True, exist_ok=True)

        proc = self._run_wrapgen(["--cc", "-o", str(out_dir), str(self.phase0_baseline_file)])
        self.assertEqual(
            proc.returncode,
            0,
            msg=(
                "omega-wrapgen failed for Phase 0 baseline fixture\n"
                f"STDOUT:\n{proc.stdout}\n"
                f"STDERR:\n{proc.stderr}"
            ),
        )

        header = out_dir / "phase0-baseline.h"
        source = out_dir / "phase0-baseline.cpp"
        self.assertTrue(header.exists(), f"Expected generated file does not exist: {header}")
        self.assertTrue(source.exists(), f"Expected generated file does not exist: {source}")

        header_content = header.read_text(encoding="utf-8")
        source_content = source.read_text(encoding="utf-8")

        self.assertIn("typedef struct Baseline__Point Baseline__Point;", header_content)
        self.assertIn("typedef struct Baseline__Drawable Baseline__Drawable;", header_content)
        self.assertIn("typedef struct Baseline__Widget * Baseline__Widget;", header_content)
        self.assertIn("OmegaArray_int BaselineWidget__get_values(", header_content)
        self.assertIn("void BaselineWidget__rename(", header_content)
        self.assertIn("long Baseline__makeId(long value);", header_content)

        # Current pointer handling is intentionally frozen here as Phase 0 baseline.
        self.assertIn("void BaselineWidget__get_native(", header_content)
        self.assertIn("void BaselineWidget__set_native(Baseline__Widget* __self,void value);", header_content)
        self.assertIn("void BaselineWidget__attach(Baseline__Widget* __self,Drawable* drawable);", header_content)

        self.assertIn("struct Baseline__Widget{Baseline::Widget obj;};", source_content)
        self.assertIn("return Baseline::makeId(", source_content)

    def test_phase0_optional_comma_quirk_is_currently_accepted(self) -> None:
        out_dir = self.work_dir / "quirk-optional-commas"
        out_dir.mkdir(parents=True, exist_ok=True)

        proc = self._run_wrapgen(["--cc", "-o", str(out_dir), str(self.quirk_optional_commas_file)])
        self.assertEqual(
            proc.returncode,
            0,
            msg=(
                "omega-wrapgen no longer accepts the documented optional-comma quirk\n"
                f"STDOUT:\n{proc.stdout}\n"
                f"STDERR:\n{proc.stderr}"
            ),
        )

        header = out_dir / "quirk-optional-commas.h"
        self.assertTrue(header.exists(), f"Expected generated file does not exist: {header}")
        header_content = header.read_text(encoding="utf-8")
        self.assertIn("struct __QuirkPair{", header_content)
        self.assertIn("QuirkConsumer__consume(", header_content)
        self.assertIn("__add(", header_content)

    def test_phase0_generated_cxx_source_compiles_for_supported_subset(self) -> None:
        source_dir = self.work_dir / "compile-input"
        out_dir = self.work_dir / "compile-output"
        source_dir.mkdir(parents=True, exist_ok=True)
        out_dir.mkdir(parents=True, exist_ok=True)

        native_header = source_dir / "CompileHeader.h"
        native_header.write_text(
            "\n".join(
                [
                    "#pragma once",
                    "class CompileWidget {",
                    "public:",
                    "    int value;",
                    "    const int id;",
                    "    CompileWidget() : value(0), id(7) {}",
                    "    void reset() { value = 0; }",
                    "    int add(int v) { return value + v; }",
                    "};",
                    "inline int compileFree(int v) { return v + 1; }",
                    "",
                ]
            ),
            encoding="utf-8",
        )

        fixture = source_dir / "phase0-compile.owrap"
        fixture.write_text(
            "\n".join(
                [
                    f'header "{native_header}"',
                    "class CompileWidget {",
                    "  value:int",
                    "  id:const int",
                    "  func reset() void",
                    "  func add(v:int) int",
                    "}",
                    "func compileFree(v:int) int",
                    "",
                ]
            ),
            encoding="utf-8",
        )

        proc = self._run_wrapgen(["--cc", "-o", str(out_dir), str(fixture)])
        self.assertEqual(
            proc.returncode,
            0,
            msg=(
                "omega-wrapgen failed for generated C++ compile fixture\n"
                f"STDOUT:\n{proc.stdout}\n"
                f"STDERR:\n{proc.stderr}"
            ),
        )

        generated_source = out_dir / "phase0-compile.cpp"
        generated_header = out_dir / "phase0-compile.h"
        self.assertTrue(generated_source.exists(), f"Expected generated source does not exist: {generated_source}")
        self.assertTrue(generated_header.exists(), f"Expected generated header does not exist: {generated_header}")

        source_compile = self._compile_generated_cpp(generated_source, out_dir / "phase0-compile.o")
        self.assertEqual(
            source_compile.returncode,
            0,
            msg=(
                "Generated C++ source did not compile\n"
                f"STDOUT:\n{source_compile.stdout}\n"
                f"STDERR:\n{source_compile.stderr}"
            ),
        )

        header_check = out_dir / "phase0-compile-header-check.cpp"
        header_check.write_text('#include "phase0-compile.h"\n', encoding="utf-8")
        header_compile = self._compile_generated_cpp(header_check, out_dir / "phase0-compile-header-check.o")
        self.assertEqual(
            header_compile.returncode,
            0,
            msg=(
                "Generated C header did not compile as a standalone C++ include\n"
                f"STDOUT:\n{header_compile.stdout}\n"
                f"STDERR:\n{header_compile.stderr}"
            ),
        )

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

    def test_unsupported_alias_syntax_fails_with_parse_diagnostic(self) -> None:
        self._assert_wrapgen_parse_fails(self.unsupported_alias_file, "Expected Keyword")

    def test_phase0_current_parser_syntax_failures_are_pinned(self) -> None:
        cases = [
            (self.syntax_missing_colon_field_file, "Expected Colon"),
            (self.syntax_missing_lbrace_class_file, "Expected LBrace"),
            (self.syntax_missing_return_type_file, "Expected Type Name"),
            (self.syntax_interface_field_file, "Expected Keyword"),
        ]
        for input_file, expected_message in cases:
            with self.subTest(input=input_file.name):
                self._assert_wrapgen_parse_fails(input_file, expected_message)



def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run omega-wrapgen integration tests")
    parser.add_argument("--omega-wrapgen", required=True, type=Path)
    parser.add_argument("--parse-test", required=True, type=Path)
    parser.add_argument("--suite-dir", required=True, type=Path)
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def main() -> int:
    global CONFIG
    args = parse_args()
    CONFIG = WrapGenTestConfig(
        omega_wrapgen=args.omega_wrapgen.resolve(),
        parse_test=args.parse_test.resolve(),
        suite_dir=args.suite_dir.resolve(),
        verbose=args.verbose,
    )

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(WrapGenIntegrationTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
