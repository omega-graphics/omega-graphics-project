#!/usr/bin/env python3
import argparse
from dataclasses import dataclass
import json
import os
import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class TestConfig:
    def __init__(
        self,
        repo_root: Path,
        autom_bin: Path,
        toolchains: Path,
        search_paths: list[Path],
        verbose: bool,
    ) -> None:
        self.repo_root = repo_root
        self.autom_bin = autom_bin
        self.toolchains = toolchains
        self.search_paths = search_paths
        self.verbose = verbose

    @property
    def has_fs_extension(self) -> bool:
        for path in self.search_paths:
            if (path / "fs.aext").exists():
                return True
        return False


CONFIG: TestConfig | None = None


def _unique_existing_paths(paths: list[Path]) -> list[Path]:
    unique: list[Path] = []
    seen: set[Path] = set()
    for path in paths:
        resolved = path.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        if resolved.exists():
            unique.append(resolved)
    return unique


@dataclass(frozen=True)
class ToolchainMatrixCase:
    name: str
    target_platform: str
    target_os: str
    toolchain_entry: dict[str, object]
    required_tools: tuple[str, ...]
    expected_cc_prefix: str
    expected_cxx_prefix: str
    expected_ar_prefix: str
    expected_so_prefix: str
    expected_exe_prefix: str
    expected_archive_ext: str
    expected_shared_ext: str
    expected_exe_name: str
    expected_compile_output_flag: str


class AutomIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if CONFIG is None:
            raise RuntimeError("CONFIG must be initialized before tests run.")
        cls.cfg = CONFIG
        if not cls.cfg.autom_bin.exists():
            raise RuntimeError(f"AUTOM binary not found: {cls.cfg.autom_bin}")
        if not cls.cfg.toolchains.exists():
            raise RuntimeError(f"Toolchain file not found: {cls.cfg.toolchains}")

    def setUp(self) -> None:
        self._tempdir = tempfile.TemporaryDirectory(prefix="autom-it-")
        self.case_dir = Path(self._tempdir.name)

    def tearDown(self) -> None:
        self._tempdir.cleanup()

    def _write(self, rel_path: str, content: str, *, cwd: Path | None = None) -> None:
        target = (cwd or self.case_dir) / rel_path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")

    def _write_json(self, rel_path: str, payload: object, *, cwd: Path | None = None) -> Path:
        target = (cwd or self.case_dir) / rel_path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        return target

    def _exe_name(self, name: str) -> str:
        if platform.system().lower().startswith("win"):
            return f"{name}.exe"
        return name

    def _run_autom(
        self,
        output_dir: str = ".",
        extra_args: list[str] | None = None,
        extra_search_paths: list[Path] | None = None,
        toolchains: Path | None = None,
        env_overrides: dict[str, str] | None = None,
        cwd: Path | None = None,
    ) -> subprocess.CompletedProcess[str]:
        cmd = [str(self.cfg.autom_bin), "--toolchains", str(toolchains or self.cfg.toolchains)]
        for search_path in self.cfg.search_paths:
            cmd.extend(["-I", str(search_path)])
        if extra_search_paths:
            for search_path in extra_search_paths:
                cmd.extend(["-I", str(search_path.resolve())])
        if extra_args:
            cmd.extend(extra_args)
        cmd.append(output_dir)
        env = os.environ.copy()
        if env_overrides:
            env.update(env_overrides)
        proc = subprocess.run(
            cmd,
            cwd=cwd or self.case_dir,
            text=True,
            capture_output=True,
            check=False,
            env=env,
        )
        if self.cfg.verbose:
            print("\n[AUTOM CMD]", " ".join(cmd))
            print("[AUTOM RC]", proc.returncode)
            print(proc.stdout)
            print(proc.stderr)
        self.assertEqual(
            proc.returncode,
            0,
            msg=f"AUTOM command failed:\nCMD: {' '.join(cmd)}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}",
        )
        return proc

    def _run_ninja(
        self,
        target: str | None = None,
        cwd: Path | None = None,
    ) -> subprocess.CompletedProcess[str]:
        if shutil.which("ninja") is None:
            self.skipTest("ninja is required for build/runtime integration coverage")
        build_dir = cwd or self.case_dir
        cmd = ["ninja", "-f", "build.ninja"]
        if target:
            cmd.append(target)
        proc = subprocess.run(
            cmd,
            cwd=build_dir,
            text=True,
            capture_output=True,
            check=False,
        )
        if self.cfg.verbose:
            print("\n[NINJA CMD]", " ".join(cmd))
            print("[NINJA RC]", proc.returncode)
            print(proc.stdout)
            print(proc.stderr)
        self.assertEqual(
            proc.returncode,
            0,
            msg=f"Ninja failed:\nCMD: {' '.join(cmd)}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}",
        )
        return proc

    def _run_binary(self, binary: Path) -> subprocess.CompletedProcess[str]:
        proc = subprocess.run(
            [str(binary)],
            cwd=binary.parent,
            text=True,
            capture_output=True,
            check=False,
        )
        if self.cfg.verbose:
            print("\n[BIN CMD]", str(binary))
            print("[BIN RC]", proc.returncode)
            print(proc.stdout)
            print(proc.stderr)
        self.assertEqual(
            proc.returncode,
            0,
            msg=f"Built binary returned non-zero status: {binary}\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}",
        )
        return proc

    def _create_tool_shims(self, names: list[str], *, cwd: Path | None = None) -> Path:
        shim_dir = (cwd or self.case_dir) / "tool-shims"
        shim_dir.mkdir(parents=True, exist_ok=True)
        for name in names:
            script = shim_dir / name
            script.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            script.chmod(0o755)
        return shim_dir

    def _require_fs_extension(self) -> None:
        self.assertTrue(
            self.cfg.has_fs_extension,
            "fs.aext was not found in any AUTOM search path. Build fs/bridge modules first.",
        )

    def test_language_features_archive_sourcegroup_and_executable(self) -> None:
        self._write(
            "AUTOM.build",
            """# Language feature coverage
project(name:"LangProj",version:"1.0")

var lib_sources = ["./src/lang.cpp"]

func pick(flag,yes,no){
    if(flag == "yes"){
        return yes
    }
    elif(flag != "yes"){
        return no
    }
    else {
        return []
    }
}

lib_sources += pick(flag:"yes",yes:["./src/util.cpp"],no:["./src/unused.cpp"])

var lib = Archive(name:"LangLib",sources:lib_sources)

foreach group_name in ["ObjA","ObjB"]{
    if(group_name == "ObjA"){
        var group_obj = SourceGroup(name:group_name,sources:["./src/sg_a.cpp"])
    }
    else {
        var group_obj = SourceGroup(name:group_name,sources:["./src/sg_b.cpp"])
    }
}

var app = Executable(name:"LangApp",sources:["./src/main.cpp"])
app.deps = ["LangLib","ObjA","ObjB"]
""",
        )
        self._write("src/util.cpp", "int util_value(){ return 20; }\n")
        self._write("src/lang.cpp", "int util_value(); int lang_value(){ return util_value() + 20; }\n")
        self._write("src/sg_a.cpp", "int sg_a(){ return 1; }\n")
        self._write("src/sg_b.cpp", "int sg_b(){ return 1; }\n")
        self._write(
            "src/main.cpp",
            "int lang_value(); int sg_a(); int sg_b(); int main(){ return (lang_value()+sg_a()+sg_b()==42)?0:1; }\n",
        )

        self._run_autom(output_dir=".")
        ninja = (self.case_dir / "build.ninja").read_text(encoding="utf-8")
        self.assertIn("build LangLib.a: ar", ninja)
        self.assertIn("build LangApp: exe", ninja)
        self.assertIn("build obj/ObjA/sg_a.o", ninja)
        self.assertIn("build obj/ObjB/sg_b.o", ninja)

        self._run_ninja()
        self._run_binary(self.case_dir / self._exe_name("LangApp"))

    def test_shared_group_script_and_install_rules(self) -> None:
        self._write(
            "AUTOM.build",
            """project(name:"KindsProj",version:"1.0")

var shared = Shared(name:"KindsShared",sources:["./src/shared.cpp"])
shared.output_dir = "dist"

var app = Executable(name:"KindsApp",sources:["./src/main.cpp"])
app.output_dir = "bin"
app.deps = ["KindsShared"]

var group = GroupTarget(name:"everything",deps:["KindsShared","KindsApp"])

var gen = Script(name:"GenStep",cmd:"./scripts/gen.py",args:["generated/out.txt"],outputs:["generated/out.txt"])
gen.desc = "Generate text output"

install_targets(targets:["KindsShared","KindsApp"],dest:"deploy/bin")
install_files(files:["./scripts/gen.py"],dest:"deploy/scripts")
""",
        )
        self._write("src/shared.cpp", "int answer(){ return 42; }\n")
        self._write("src/main.cpp", "int main(){ return 0; }\n")
        self._write(
            "scripts/gen.py",
            "import pathlib,sys\nout=pathlib.Path(sys.argv[1])\nout.parent.mkdir(parents=True,exist_ok=True)\nout.write_text('ok\\n',encoding='utf-8')\n",
        )

        self._run_autom(output_dir=".")
        ninja = (self.case_dir / "build.ninja").read_text(encoding="utf-8")
        self.assertRegex(ninja, r"build dist/KindsShared\.[A-Za-z0-9_]+: so")
        self.assertIn("build bin/KindsApp: exe", ninja)
        self.assertIn("build everything: phony KindsShared KindsApp", ninja)
        self.assertRegex(ninja, r"build generated/out\.txt\s*:script\s+\./scripts/gen\.py")

        self._run_ninja(target="generated/out.txt")
        self.assertEqual((self.case_dir / "generated/out.txt").read_text(encoding="utf-8"), "ok\n")

        install_data = json.loads((self.case_dir / "AUTOMINSTALL").read_text(encoding="utf-8"))
        self.assertEqual(len(install_data), 2)
        target_rule = next(item for item in install_data if item["type"] == "target")
        file_rule = next(item for item in install_data if item["type"] == "file")
        self.assertEqual(target_rule["dest"], "deploy/bin")
        self.assertTrue(any(path.startswith("bin/KindsApp") for path in target_rule["targets"]))
        self.assertTrue(any(path.startswith("dist/KindsShared.") for path in target_rule["targets"]))
        self.assertEqual(file_rule["dest"], "deploy/scripts")
        self.assertIn("./scripts/gen.py", file_rule["sources"])

    def test_import_subdir_fs_find_program_and_config_file(self) -> None:
        self._require_fs_extension()
        self._write(
            "AUTOM.build",
            """import "fs"
import "./local/tools"

project(name:"FeatureProj",version:"1.0")

var VALUE = "21"
var sources = ["./src/main.cpp", GENERATED_SRC]

if(fs_exists(path:"./templates/generated.in") == true){
    fs_mkdir(path:"./src")
    config_file(in:"./templates/generated.in",out:"./src/generated.cpp")
}

var py = find_program(cmd:"python3")
print(msg:py)

subdir(path:"./child")

var app = Executable(name:"FeatureApp",sources:sources)
app.output_dir = "bin"
app.deps += ["ChildLib"]
install_targets(targets:["FeatureApp","ChildLib"],dest:"deploy/bin")
""",
        )
        self._write(
            "local/tools.autom",
            'var GENERATED_SRC = "./src/generated.cpp"\n',
        )
        self._write("child/AUTOM.build", 'var child = Archive(name:"ChildLib",sources:["./child.cpp"])\n')
        self._write("child/child.cpp", "int child(){ return 21; }\n")
        self._write(
            "src/main.cpp",
            "int generated(); int child(); int main(){ return (generated()+child()==42)?0:1; }\n",
        )
        self._write("templates/generated.in", "int generated(){ return @VALUE@; }\n")

        self._run_autom(output_dir=".")
        generated_cpp = (self.case_dir / "src/generated.cpp")
        self.assertTrue(generated_cpp.exists())
        self.assertIn("21", generated_cpp.read_text(encoding="utf-8"))

        ninja = (self.case_dir / "build.ninja").read_text(encoding="utf-8")
        self.assertIn("build ChildLib.a: ar", ninja)
        self.assertIn("build bin/FeatureApp: exe", ninja)

        self._run_ninja()
        self._run_binary(self.case_dir / "bin" / self._exe_name("FeatureApp"))

    def test_direct_load_and_fs_glob(self) -> None:
        self._require_fs_extension()
        self._write(
            "AUTOM.build",
            """load "fs.aext"
project(name:"LoadProj",version:"1.0")
var srcs = fs_glob(path:"./src/*.cpp")
var app = Executable(name:"LoadApp",sources:srcs)
app.output_dir = fs_abspath(path:"./bin")
""",
        )
        self._write("src/main.cpp", "int helper(); int main(){ return helper()==7 ? 0 : 1; }\n")
        self._write("src/helper.cpp", "int helper(){ return 7; }\n")

        self._run_autom(output_dir=".")
        self._run_ninja()
        self._run_binary(self.case_dir / "bin" / self._exe_name("LoadApp"))

    def test_unresolved_dependency_reports_error(self) -> None:
        self._write(
            "AUTOM.build",
            """project(name:"BrokenProj",version:"1.0")
var app = Executable(name:"BrokenApp",sources:["./src/main.cpp"])
app.deps = ["DoesNotExist"]
""",
        )
        self._write("src/main.cpp", "int main(){ return 0; }\n")

        proc = self._run_autom(output_dir=".")
        self.assertIn("unresolved dependencies", proc.stdout)
        self.assertFalse((self.case_dir / "build.ninja").exists())

    def test_target_with_no_sources_reports_error(self) -> None:
        self._write(
            "AUTOM.build",
            """project(name:"NoSourceProj",version:"1.0")
var app = Executable(name:"NoSourceApp",sources:[])
""",
        )

        proc = self._run_autom(output_dir=".")
        self.assertIn("has no sources", proc.stdout)
        self.assertFalse((self.case_dir / "build.ninja").exists())

    def test_script_target_requires_outputs(self) -> None:
        self._write(
            "AUTOM.build",
            """project(name:"ScriptProj",version:"1.0")
var gen = Script(name:"Gen",cmd:"./scripts/gen.py",args:["./out.txt"],outputs:[])
""",
        )
        self._write("scripts/gen.py", "print('hello')\n")

        proc = self._run_autom(output_dir=".")
        self.assertIn("Script targets must have at least 1 output file", proc.stdout)
        self.assertFalse((self.case_dir / "build.ninja").exists())

    def test_import_missing_interface_reports_error(self) -> None:
        self._write(
            "AUTOM.build",
            """import "./missing/module"
project(name:"ImportMissingProj",version:"1.0")
var app = Executable(name:"ImportMissingApp",sources:["./src/main.cpp"])
""",
        )
        self._write("src/main.cpp", "int main(){ return 0; }\n")

        proc = self._run_autom(output_dir=".")
        self.assertIn("Cannot import file", proc.stdout)
        self.assertFalse((self.case_dir / "build.ninja").exists())

    def test_xcode_generation_includes_native_group_and_script_targets(self) -> None:
        self._write(
            "AUTOM.build",
            """project(name:"XcodeProj",version:"1.0")

var lib = Archive(name:"XLib",sources:["./src/lib.cpp"])
var app = Executable(name:"XApp",sources:["./src/main.cpp"])
app.deps = ["XLib"]

var run = Script(name:"RunGen",cmd:"./scripts/gen.py",args:["generated/out.txt"],outputs:["generated/out.txt"])
var group = GroupTarget(name:"AllTargets",deps:["XApp","RunGen"])
""",
        )
        self._write("src/lib.cpp", "int lib_value(){ return 42; }\n")
        self._write("src/main.cpp", "int lib_value(); int main(){ return lib_value()==42 ? 0 : 1; }\n")
        self._write(
            "scripts/gen.py",
            "import pathlib,sys\nout=pathlib.Path(sys.argv[1])\nout.parent.mkdir(parents=True,exist_ok=True)\nout.write_text('ok\\n',encoding='utf-8')\n",
        )

        self._run_autom(output_dir=".", extra_args=["--xcode", "--new-build"])
        pbxproj = self.case_dir / "XcodeProj.xcodeproj" / "project.pbxproj"
        self.assertTrue(pbxproj.exists())
        text = pbxproj.read_text(encoding="utf-8")
        self.assertIn("PBXNativeTarget", text)
        self.assertIn("PBXAggregateTarget", text)
        self.assertIn("RunGen", text)
        self.assertIn("AllTargets", text)
        self.assertIn("PBXShellScriptBuildPhase", text)

        xcodebuild = shutil.which("xcodebuild")
        if xcodebuild is not None:
            proc = subprocess.run(
                [xcodebuild, "-list", "-project", str(self.case_dir / "XcodeProj.xcodeproj")],
                text=True,
                capture_output=True,
                check=False,
                cwd=self.case_dir,
            )
            self.assertEqual(
                proc.returncode,
                0,
                msg=f"xcodebuild -list failed:\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}",
            )

    def test_sln_generation_smoke(self) -> None:
        self._write(
            "AUTOM.build",
            """project(name:"SlnProj",version:"1.0")
var lib = Archive(name:"SlnLib",sources:["./src/lib.cpp"])
var app = Executable(name:"SlnApp",sources:["./src/main.cpp"])
app.deps = ["SlnLib"]
""",
        )
        self._write("src/lib.cpp", "int sln_val(){ return 1; }\n")
        self._write("src/main.cpp", "int sln_val(); int main(){ return sln_val() == 1 ? 0 : 1; }\n")

        self._run_autom(output_dir=".", extra_args=["--sln"])
        self.assertTrue((self.case_dir / "SlnProj.sln").exists())
        self.assertTrue((self.case_dir / "SlnLib.vcxproj").exists())
        self.assertTrue((self.case_dir / "SlnApp.vcxproj").exists())

    def test_custom_target_flags_and_output_overrides(self) -> None:
        self._write(
            "AUTOM.build",
            """project(name:"FlagsProj",version:"1.0")
var app = Executable(name:"FlagsApp",sources:["./src/main.cpp"])
app.cflags = ["-Wall","-Wextra"]
app.include_dirs = ["./include","./vendor/include"]
app.libs = ["pthread","m"]
app.lib_dirs = ["./lib","./vendor/lib"]
app.output_dir = "out/bin"
app.output_ext = "customexe"
""",
        )
        self._write("src/main.cpp", "int main(){ return 0; }\n")

        toolchains_path = self._write_json(
            "toolchains.json",
            [
                {
                    "name": "GCC",
                    "type": "cfamily",
                    "platforms": ["macos"],
                    "progs": {
                        "cc": "gcc-shim",
                        "cxx": "g++-shim",
                        "objc": "objc-shim",
                        "objcxx": "objcxx-shim",
                        "ld_so": "ld-shim",
                        "ld_exe": "ld-shim",
                        "ar": "ar-shim",
                    },
                    "flags": {
                        "define": "-D",
                        "include_dir": "-I",
                        "lib": "-l",
                        "lib_dir": "-L",
                        "framework": "-framework",
                        "framework_dir": "-F",
                        "compile_output": "-o",
                        "link_output": "-o",
                        "compile": "-c",
                        "shared": "",
                        "executable": "",
                        "strip_lib_prefix": False,
                    },
                }
            ],
        )
        shim_dir = self._create_tool_shims(
            ["gcc-shim", "g++-shim", "objc-shim", "objcxx-shim", "ld-shim", "ar-shim"]
        )
        sep = ";" if platform.system().lower().startswith("win") else ":"
        env_path = f"{shim_dir}{sep}{os.environ.get('PATH', '')}"

        self._run_autom(
            output_dir=".",
            toolchains=toolchains_path,
            env_overrides={"PATH": env_path},
            extra_args=["--platform", "macos", "--os", "linux", "--ninja"],
        )
        ninja = (self.case_dir / "build.ninja").read_text(encoding="utf-8")
        self.assertIn("build out/bin/FlagsApp.customexe: exe", ninja)
        self.assertIn("CFLAGS=-Wall -Wextra", ninja)
        self.assertIn("INCLUDE_DIRS=-I./include -I./vendor/include", ninja)
        self.assertIn("LIBS=-lpthread -lm", ninja)
        self.assertIn("LIB_DIRS=-L./lib -L./vendor/lib", ninja)

    def test_toolchain_matrix_generation_signatures(self) -> None:
        cases = [
            ToolchainMatrixCase(
                name="gnu-linux",
                target_platform="macos",
                target_os="linux",
                toolchain_entry={
                    "name": "GCC",
                    "type": "cfamily",
                    "platforms": ["macos"],
                    "progs": {
                        "cc": "gcc-shim",
                        "cxx": "g++-shim",
                        "objc": "objc-shim",
                        "objcxx": "objcxx-shim",
                        "ld_so": "ld-shim",
                        "ld_exe": "ld-shim",
                        "ar": "ar-shim",
                    },
                    "flags": {
                        "define": "-D",
                        "include_dir": "-I",
                        "lib": "-l",
                        "lib_dir": "-L",
                        "framework": "-framework",
                        "framework_dir": "-F",
                        "compile_output": "-o",
                        "link_output": "-o",
                        "compile": "-c",
                        "shared": "",
                        "executable": "",
                        "strip_lib_prefix": False,
                    },
                },
                required_tools=("gcc-shim", "g++-shim", "objc-shim", "objcxx-shim", "ld-shim", "ar-shim"),
                expected_cc_prefix="command = gcc-shim",
                expected_cxx_prefix="command = g++-shim",
                expected_ar_prefix="command = ar-shim",
                expected_so_prefix="command = ld-shim",
                expected_exe_prefix="command = ld-shim",
                expected_archive_ext="a",
                expected_shared_ext="so",
                expected_exe_name="CoreApp",
                expected_compile_output_flag="-o$out",
            ),
            ToolchainMatrixCase(
                name="llvm-linux",
                target_platform="macos",
                target_os="linux",
                toolchain_entry={
                    "name": "LLVM",
                    "type": "cfamily",
                    "platforms": ["macos"],
                    "progs": {
                        "cc": "clang-shim",
                        "cxx": "clang++-shim",
                        "objc": "objc-shim",
                        "objcxx": "objcxx-shim",
                        "ld_so": "ld.lld-shim",
                        "ld_exe": "ld.lld-shim",
                        "ar": "llvm-ar-shim",
                    },
                    "flags": {
                        "define": "-D",
                        "include_dir": "-I",
                        "lib": "-l",
                        "lib_dir": "-L",
                        "framework": "-framework",
                        "framework_dir": "-F",
                        "compile_output": "-o",
                        "link_output": "-o",
                        "compile": "-c",
                        "shared": "",
                        "executable": "",
                        "strip_lib_prefix": False,
                    },
                },
                required_tools=(
                    "clang-shim",
                    "clang++-shim",
                    "objc-shim",
                    "objcxx-shim",
                    "ld.lld-shim",
                    "llvm-ar-shim",
                ),
                expected_cc_prefix="command = clang-shim",
                expected_cxx_prefix="command = clang++-shim",
                expected_ar_prefix="command = llvm-ar-shim",
                expected_so_prefix="command = ld.lld-shim",
                expected_exe_prefix="command = ld.lld-shim",
                expected_archive_ext="a",
                expected_shared_ext="so",
                expected_exe_name="CoreApp",
                expected_compile_output_flag="-o$out",
            ),
            ToolchainMatrixCase(
                name="llvm-windows",
                target_platform="windows",
                target_os="windows",
                toolchain_entry={
                    "name": "LLVM",
                    "type": "cfamily",
                    "platforms": ["windows"],
                    "progs": {
                        "cc": "clang-cl-shim",
                        "cxx": "clang-cl-shim",
                        "objc": "objc-shim",
                        "objcxx": "objcxx-shim",
                        "ld_so": "lld-link-shim",
                        "ld_exe": "lld-link-shim",
                        "ar": "llvm-lib-shim",
                    },
                    "flags": {
                        "define": "/D",
                        "include_dir": "/I",
                        "lib": "",
                        "lib_dir": "/LIBPATH:",
                        "framework": "-framework",
                        "framework_dir": "-F",
                        "compile_output": "/Fo",
                        "link_output": "/out:",
                        "compile": "/c",
                        "shared": "/dll",
                        "executable": "",
                        "strip_lib_prefix": False,
                    },
                },
                required_tools=("clang-cl-shim", "objc-shim", "objcxx-shim", "lld-link-shim", "llvm-lib-shim"),
                expected_cc_prefix="command = clang-cl-shim",
                expected_cxx_prefix="command = clang-cl-shim",
                expected_ar_prefix="command = llvm-lib-shim",
                expected_so_prefix="command = lld-link-shim /dll",
                expected_exe_prefix="command = lld-link-shim",
                expected_archive_ext="lib",
                expected_shared_ext="dll",
                expected_exe_name="CoreApp.exe",
                expected_compile_output_flag="/Fo$out",
            ),
            ToolchainMatrixCase(
                name="msvc-windows",
                target_platform="windows",
                target_os="windows",
                toolchain_entry={
                    "name": "MSVC",
                    "type": "cfamily",
                    "platforms": ["windows"],
                    "progs": {
                        "cc": "cl-shim",
                        "cxx": "cl-shim",
                        "objc": "objc-shim",
                        "objcxx": "objcxx-shim",
                        "ld_so": "link-shim",
                        "ld_exe": "link-shim",
                        "ar": "lib-shim",
                    },
                    "flags": {
                        "define": "/D",
                        "include_dir": "/I",
                        "lib": "",
                        "lib_dir": "/LIBPATH:",
                        "framework": "-framework",
                        "framework_dir": "-F",
                        "compile_output": "/Fo",
                        "link_output": "/out:",
                        "compile": "/c",
                        "shared": "/LD /link",
                        "executable": "/link",
                        "strip_lib_prefix": False,
                    },
                },
                required_tools=("cl-shim", "objc-shim", "objcxx-shim", "link-shim", "lib-shim"),
                expected_cc_prefix="command = cl-shim",
                expected_cxx_prefix="command = cl-shim",
                expected_ar_prefix="command = lib-shim",
                expected_so_prefix="command = link-shim /LD /link",
                expected_exe_prefix="command = link-shim /link",
                expected_archive_ext="lib",
                expected_shared_ext="dll",
                expected_exe_name="CoreApp.exe",
                expected_compile_output_flag="/Fo$out",
            ),
        ]

        for case in cases:
            with self.subTest(case=case.name):
                case_dir = self.case_dir / case.name
                case_dir.mkdir(parents=True, exist_ok=True)

                self._write(
                    "AUTOM.build",
                    """project(name:"MatrixProj",version:"1.0")
var lib = Archive(name:"CoreLib",sources:["./src/lib.cpp"])
var shared = Shared(name:"CoreShared",sources:["./src/shared.cpp"])
var app = Executable(name:"CoreApp",sources:["./src/main.cpp"])
app.deps = ["CoreLib","CoreShared"]
""",
                    cwd=case_dir,
                )
                self._write("src/lib.cpp", "int libv(){ return 1; }\n", cwd=case_dir)
                self._write("src/shared.cpp", "int sharedv(){ return 2; }\n", cwd=case_dir)
                self._write(
                    "src/main.cpp",
                    "int libv(); int sharedv(); int main(){ return (libv()+sharedv()==3)?0:1; }\n",
                    cwd=case_dir,
                )

                toolchains_path = self._write_json("toolchains.json", [case.toolchain_entry], cwd=case_dir)
                shim_dir = self._create_tool_shims(list(case.required_tools), cwd=case_dir)
                sep = ";" if platform.system().lower().startswith("win") else ":"
                env_path = f"{shim_dir}{sep}{os.environ.get('PATH', '')}"

                self._run_autom(
                    output_dir=".",
                    toolchains=toolchains_path,
                    env_overrides={"PATH": env_path},
                    cwd=case_dir,
                    extra_args=[
                        "--platform",
                        case.target_platform,
                        "--os",
                        case.target_os,
                        "--ninja",
                    ],
                )

                ninja = (case_dir / "build.ninja").read_text(encoding="utf-8")
                toolchain_ninja = (case_dir / "toolchain.ninja").read_text(encoding="utf-8")

                self.assertIn(f"build CoreLib.{case.expected_archive_ext}: ar", ninja)
                self.assertIn(f"build CoreShared.{case.expected_shared_ext}: so", ninja)
                self.assertIn(f"build {case.expected_exe_name}: exe", ninja)

                self.assertIn("rule cc", toolchain_ninja)
                self.assertIn("rule cxx", toolchain_ninja)
                self.assertIn("rule so", toolchain_ninja)
                self.assertIn("rule exe", toolchain_ninja)
                self.assertIn("rule ar", toolchain_ninja)

                self.assertIn(case.expected_cc_prefix, toolchain_ninja)
                self.assertIn(case.expected_cxx_prefix, toolchain_ninja)
                self.assertIn(case.expected_ar_prefix, toolchain_ninja)
                self.assertIn(case.expected_so_prefix, toolchain_ninja)
                self.assertIn(case.expected_exe_prefix, toolchain_ninja)
                self.assertIn(case.expected_compile_output_flag, toolchain_ninja)


def _detect_default_autom_bin(repo_root: Path) -> Path:
    candidates = [
        repo_root / "build/bin/autom",
        repo_root / "autom/bin/autom",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def _parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description="Run AUTOM integration tests.")
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=repo_root,
        help="Omega Graphics project root.",
    )
    parser.add_argument(
        "--autom",
        type=Path,
        default=_detect_default_autom_bin(repo_root),
        help="Path to the autom executable.",
    )
    parser.add_argument(
        "--toolchains",
        type=Path,
        default=repo_root / "autom/tools/default_toolchains.json",
        help="Path to default_toolchains.json.",
    )
    parser.add_argument(
        "--interface-dir",
        type=Path,
        action="append",
        default=[],
        help="Additional AUTOM interface search path (-I). Can be repeated.",
    )
    parser.add_argument(
        "--extension-dir",
        type=Path,
        action="append",
        default=[],
        help="Additional AUTOM extension search path (-I). Can be repeated.",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print subprocess output for each test.",
    )
    return parser.parse_args()


def main() -> int:
    global CONFIG
    args = _parse_args()
    repo_root = args.repo_root.resolve()

    interface_defaults = [repo_root / "autom/modules"]
    extension_defaults = [
        repo_root / "build/stdlib",
        repo_root / "build/modules",
        repo_root / "autom/modules",
    ]

    interface_dirs = _unique_existing_paths(interface_defaults + [p.resolve() for p in args.interface_dir])
    extension_dirs = _unique_existing_paths(extension_defaults + [p.resolve() for p in args.extension_dir])
    search_paths = _unique_existing_paths(interface_dirs + extension_dirs)

    CONFIG = TestConfig(
        repo_root=repo_root,
        autom_bin=args.autom.resolve(),
        toolchains=args.toolchains.resolve(),
        search_paths=search_paths,
        verbose=args.verbose,
    )

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(AutomIntegrationTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
