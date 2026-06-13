import os
import shutil
import argparse
import sys



parser = argparse.ArgumentParser()
parser.add_argument("-F",type=str)
parser.add_argument("--name",type=str)
parser.add_argument("--current_version",type=str)
parser.add_argument("--check_links",dest="check_links",action="store_const",const=True,default=False)
parser.add_argument("--symlink-other-dirs",dest="symlink_other_dirs",nargs="+",type=str)


def _replace_symlink(target, link_path):
    """(Re)create link_path -> target, removing any stale/absolute link first.

    `target` MUST be relative to the directory that contains link_path so the
    framework stays self-contained when the whole .framework is copied into an
    app bundle (cp -R preserves the relative link verbatim)."""
    if os.path.islink(link_path):
        os.remove(link_path)
    elif os.path.exists(link_path):
        if os.path.isdir(link_path):
            shutil.rmtree(link_path)
        else:
            os.remove(link_path)
    os.symlink(target, link_path)


def main(_args:"list[str]"):
    args = parser.parse_args(_args)

    framework_main_dir = args.F
    versions_dir = os.path.join(framework_main_dir, "Versions")
    framework_current_version_dir = os.path.join(versions_dir, args.current_version)

    if(args.check_links):
        sys.stdout.write("{}".format(os.path.exists(framework_main_dir + "/" + args.name) and os.path.exists(framework_main_dir + "/" + "Resources") and os.path.exists(framework_main_dir + "/" + "Versions" + "/" + "Current")))
        return

    # Versions/Current -> <current_version>. RELATIVE (a sibling under
    # Versions/), and ALWAYS refreshed so a version bump (e.g. 0.5 -> 0.6)
    # repoints Current instead of leaving it stuck on the old version.
    _replace_symlink(args.current_version, os.path.join(versions_dir, "Current"))

    # Prune stale version dirs left behind by earlier builds at a different
    # version. Only the current version dir + the Current symlink survive, so
    # the bundle never ships a stale binary (which would still reference the
    # pre-rename dylib names).
    for entry in os.listdir(versions_dir):
        if entry in (args.current_version, "Current"):
            continue
        stale = os.path.join(versions_dir, entry)
        if os.path.islink(stale):
            os.remove(stale)
        elif os.path.isdir(stale):
            shutil.rmtree(stale)

    # Top-level framework symlinks resolve THROUGH Versions/Current, expressed
    # relative to the framework root so they stay valid inside any bundle the
    # framework is copied into.
    _replace_symlink(os.path.join("Versions", "Current", args.name),
                     os.path.join(framework_main_dir, args.name))

    if os.path.exists(os.path.join(framework_current_version_dir, "Resources")):
        _replace_symlink(os.path.join("Versions", "Current", "Resources"),
                         os.path.join(framework_main_dir, "Resources"))

    if(args.symlink_other_dirs):
        for d in args.symlink_other_dirs:
            if os.path.exists(os.path.join(framework_current_version_dir, d)):
                _replace_symlink(os.path.join("Versions", "Current", d),
                                 os.path.join(framework_main_dir, d))

    os.system(f"install_name_tool -id {'@rpath/' + args.name + '.framework/Versions/' + args.current_version + '/' + args.name} {framework_main_dir + '/' + args.name}")

if(__name__ == "__main__"):
    main(sys.argv)
