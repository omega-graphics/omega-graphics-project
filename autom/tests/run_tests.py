#!/usr/bin/env python3
import argparse
import json
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
        if shutil.which("ninja") is None:
            raise RuntimeError("ninja is required to run AUTOM integration tests.")

    def setUp(self) -> None:
        self._tempdir = tempfile.TemporaryDirectory(prefix="autom-it-")
        self.case_dir = Path(self._tempdir.name)

    def tearDown(self) -> None:
        self._tempdir.cleanup()

    def _write(self, rel_path: str, content: str) -> None:
        target = self.case_dir / rel_path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")

    def _exe_name(self, name: str) -> str:
        if platform.system().lower().startswith("win"):
            return f"{name}.exe"
        return name

    def _run_autom(
        self,
        output_dir: str = ".",
        extra_args: list[str] | None = None,
        extra_search_paths: list[Path] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        cmd = [str(self.cfg.autom_bin), "--toolchains", str(self.cfg.toolchains)]
        for search_path in self.cfg.search_paths:
            cmd.extend(["-I", str(search_path)])
        if extra_search_paths:
            for search_path in extra_search_paths:
                cmd.extend(["-I", str(search_path.resolve())])
        if extra_args:
            cmd.extend(extra_args)
        cmd.append(output_dir)
        proc = subprocess.run(
            cmd,
            cwd=self.case_dir,
            text=True,
            capture_output=True,
            check=False,
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
