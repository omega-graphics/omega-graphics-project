import argparse
import os
import shutil
import subprocess
import sys
import AppleFramework


# Mach-O magic numbers (32/64-bit, both endians, and fat/universal).
_MACHO_MAGICS = {
    b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf", b"\xfe\xed\xfa\xce",
    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
}


def _is_macho(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) in _MACHO_MAGICS
    except OSError:
        return False


def _build_tree_rpaths(macho_path, build_dir):
    """LC_RPATH entries of macho_path that point into the build tree."""
    try:
        out = subprocess.run(["otool", "-l", macho_path],
                             capture_output=True, text=True).stdout
    except OSError:
        return []
    found, lines = [], out.splitlines()
    for i, line in enumerate(lines):
        if "LC_RPATH" not in line:
            continue
        for j in range(i + 1, min(i + 4, len(lines))):
            s = lines[j].strip()
            if s.startswith("path "):
                p = s[5:].split(" (offset", 1)[0]
                if p.startswith(build_dir):
                    found.append(p)
                break
    return found


def strip_build_rpaths(bundle_dir, build_dir):
    """Drop every LC_RPATH pointing into the build tree from each Mach-O copy in
    the bundle, so the embedded dylibs/frameworks are relocatable rather than
    falling back to build/lib. On arm64 install_name_tool re-applies a valid
    ad-hoc signature to each Mach-O in place. Only the bundle's own copies are
    touched; the build-tree originals are left alone. Returns the set of nested
    .framework dirs whose binary was modified (so their bundle seal can be
    refreshed by the caller)."""
    touched_frameworks = set()
    for dirpath, _dirnames, filenames in os.walk(bundle_dir):
        for fn in filenames:
            fp = os.path.join(dirpath, fn)
            if os.path.islink(fp) or not _is_macho(fp):
                continue
            rpaths = _build_tree_rpaths(fp, build_dir)
            if not rpaths:
                continue
            for rp in rpaths:
                subprocess.run(["install_name_tool", "-delete_rpath", rp, fp])
            marker = fp.find(".framework/")
            if marker != -1:
                touched_frameworks.add(fp[:marker + len(".framework")])
    return touched_frameworks


parser = argparse.ArgumentParser()
parser.add_argument("--sig",type=str)
parser.add_argument("--code",type=str)
#parser.add_argument("--output_dir",type=str)
parser.add_argument("--framework",dest="framework",action="store_const",const=True,default=False)
parser.add_argument("--strip-build-rpaths",dest="strip_build_rpaths",type=str,default=None)
args = parser.parse_known_args()

def _clean_cstemp(root):
    """Remove leftover *.cstemp files. codesign writes the new signature to a
    <name>.cstemp beside the target and atomically renames it into place; an
    interrupted/raced sign leaves the temp behind. A stray .cstemp in
    Contents/MacOS/ then makes the next verify fail ("not signed at all"
    subcomponent), so sweep them before re-signing."""
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            if fn.endswith(".cstemp"):
                try:
                    os.remove(os.path.join(dirpath, fn))
                except OSError:
                    pass


if(args[0].framework):
    AppleFramework.main(args[1])

# For app bundles: make the embedded dylibs/frameworks relocatable by removing
# any build-tree rpath before the bundle is sealed. Re-seal any nested framework
# whose binary changed so its CodeResources stay consistent, then sign the app.
if(args[0].strip_build_rpaths):
    _clean_cstemp(args[0].code)
    _touched_frameworks = strip_build_rpaths(args[0].code, args[0].strip_build_rpaths)
    for _fw in _touched_frameworks:
        os.system("codesign --force -s " + args[0].sig + " " + _fw)

os.system("codesign --force  -s " + args[0].sig + " --verbose=3 " + args[0].code)
# output_file = os.path.join(args.output_dir,os.path.basename(args.code))
# if(output_file):
#     os.remove(output_file)
# shutil.copyfile(args.code,args.output_dir)
