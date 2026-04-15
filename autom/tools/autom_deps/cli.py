from __future__ import annotations

import argparse
import sys


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
    parser.add_argument("--cmake", dest="cmake")
    parser.add_argument("--verbose", action="store_const", const=True, default=False)
    parser.add_argument("--quiet", action="store_const", const=True, default=False)
    parser.add_argument("--json-log", dest="json_log", action="store_const", const=True, default=False)
    parser.add_argument("--progress", action="store_const", const=True, default=False)
    parser.add_argument("--no-progress", dest="no_progress", action="store_const", const=True, default=False)
    parser.add_argument("--no-resume", dest="no_resume", action="store_const", const=True, default=False)
    parser.add_argument("--single-stream", dest="single_stream", action="store_const", const=True, default=False)

    args = parser.parse_args(argv)

    if args.verify and args.clean:
        parser.error("--verify cannot be combined with --clean")
    if args.verify and (args.refresh or args.refresh_all):
        parser.error("--verify cannot be combined with --refresh or --refresh-all")
    if args.update and (args.verify or args.dry_run or args.refresh or args.refresh_all or args.clean):
        parser.error("--sync cannot be combined with phase 3 execution mode flags")
    if args.clean and args.print_resolved:
        parser.error("--clean cannot be combined with --print-resolved")
    if args.print_resolved and args.cmake:
        parser.error("--print-resolved cannot be combined with --cmake")
    if args.verbose and args.quiet:
        parser.error("--verbose cannot be combined with --quiet")
    if args.progress and args.no_progress:
        parser.error("--progress cannot be combined with --no-progress")

    args.refresh = list(dict.fromkeys(args.refresh))
    args.clean = list(dict.fromkeys(args.clean))
    args.show_progress = args.progress or (not args.no_progress and not args.quiet and not args.json_log and sys.stdout.isatty())
    return args
