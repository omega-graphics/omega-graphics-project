from __future__ import annotations

import argparse


def parse_args(argv: list[str] | None):
    parser = argparse.ArgumentParser(
        prog="autom-deps",
        description="AUTOM Project Dependency Manager (automates third-party dependency fetching and project bootstrap).",
    )
    parser.add_argument("--exec", action="store_const", const=True, default=True)
    parser.add_argument("--sync", dest="update", action="store_const", const=True, default=False)
    parser.add_argument(
        "--target",
        nargs="?",
        choices=["windows", "macos", "linux", "ios", "android"],
        dest="target",
    )
    parser.add_argument("--verify", action="store_const", const=True, default=False)
    parser.add_argument("--dry-run", dest="dry_run", action="store_const", const=True, default=False)
    parser.add_argument("--refresh", action="append", default=[])
    parser.add_argument("--refresh-all", dest="refresh_all", action="store_const", const=True, default=False)
    parser.add_argument("--clean", action="append", default=[])
    parser.add_argument("--resolve-stable", dest="resolve_stable", action="store_const", const=True, default=False)
    parser.add_argument("--print-resolved", dest="print_resolved")

    args = parser.parse_args(argv)

    if args.verify and args.clean:
        parser.error("--verify cannot be combined with --clean")
    if args.verify and (args.refresh or args.refresh_all):
        parser.error("--verify cannot be combined with --refresh or --refresh-all")
    if args.update and (args.verify or args.dry_run or args.refresh or args.refresh_all or args.clean):
        parser.error("--sync cannot be combined with phase 3 execution mode flags")
    if args.clean and args.print_resolved:
        parser.error("--clean cannot be combined with --print-resolved")

    args.refresh = list(dict.fromkeys(args.refresh))
    args.clean = list(dict.fromkeys(args.clean))
    return args
