#!/usr/bin/env python3
"""viz-calibrate — (re)calibrate the omega-debugviz capture region.

This is the mechanism behind the /calibrate command
(.claude/commands/calibrate.md). Run it once after the graphics app has loaded
so the agent knows which screen region to capture; run it again any time the app
moves, the resolution changes, or a laptop is docked/undocked. Keep the app open
while calibrating — you should not have to close and reopen it.

It is a thin, cross-platform wrapper around `debugviz.py calibrate` that resolves
the tool path relative to the repo, so it can be invoked from anywhere. It is
written in Python (not shell) on purpose: visual debugging has to work on native
Windows too, and the repo git-ignores *.sh.

Usage:
    python3 .claude/hooks/viz-calibrate.py                   # interactive drag
    python3 .claude/hooks/viz-calibrate.py --region X Y W H  # set coords directly

NOTE: the drag overlay (and capture) cannot run through WSL — there is no host
display. On Windows-via-WSL, set --region by hand and screenshot manually.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]  # .claude/hooks/ -> repo root
TOOL = REPO_ROOT / "utils" / "omega-debugviz" / "debugviz.py"


def main() -> int:
    if not TOOL.exists():
        print(f"viz-calibrate: cannot find omega-debugviz at {TOOL}", file=sys.stderr)
        return 1
    return subprocess.run(
        [sys.executable, str(TOOL), "calibrate", *sys.argv[1:]]
    ).returncode


if __name__ == "__main__":
    sys.exit(main())
