#!/usr/bin/env python3
"""omega-debugviz — region-based screen capture for visual debugging of Omega graphics apps.

Testing a graphics application is not finished until you have *seen* its output:
a passing test suite proves the code ran, not that the frame rendered correctly.
This tool gives an agent (or a human) a deterministic way to look at the running
app by capturing a single, user-defined rectangular region of the screen.

Why a fixed region instead of a specific window:
  - Works identically on Windows, macOS, X11 and Wayland.
  - Needs no window-enumeration APIs and hits no Wayland window-discovery
    permission issues.
  - Works with games, editors, launchers and custom rendering engines.
  - Fast — only a subsection of the screen is read.

The region is stored as JSON ({x, y, width, height}) and reused for every
capture. It lives in screen coordinates, so it must be (re)calibrated whenever
the app moves, the resolution changes, or a laptop is docked/undocked.

Commands:
    calibrate                    drag a rectangle around the app (needs a
                                 display + tkinter), then store the region
    calibrate --region X Y W H   store a region directly, no GUI (use under
                                 WSL/headless or when coords are already known)
    capture [-o FILE]            grab the stored region to a PNG (needs `mss`)
    show [--json]                print the stored region
    where                        print the path of the region file

Dependencies:
  - `mss` (pip install mss) — required for `capture`.
  - `tkinter` (Python standard library; on Debian/Ubuntu `apt install
    python3-tk`) — required only for interactive `calibrate`. Not needed for
    `--region`, `show` or `where`.

NOTE: capture does not work through WSL — there is no host display to grab. On
Windows-via-WSL the user must take a screenshot manually (see AGENTS.md).
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path
from typing import Optional

# --------------------------------------------------------------------------- #
# Layout / configuration
# --------------------------------------------------------------------------- #

SELF_DIR = Path(__file__).resolve().parent
DEFAULT_REGION_FILE = SELF_DIR / "region.json"
DEFAULT_CAPTURE_DIR = SELF_DIR / "captures"

REGION_KEYS = ("x", "y", "width", "height")


def region_file() -> Path:
    """Where the calibrated region lives. Overridable for multi-app / CI setups."""
    env = os.environ.get("OMEGA_DEBUGVIZ_REGION")
    return Path(env).expanduser().resolve() if env else DEFAULT_REGION_FILE


def rel(p: Path) -> str:
    """Path relative to cwd when possible, for friendlier messages."""
    try:
        return str(p.relative_to(Path.cwd()))
    except ValueError:
        return str(p)


# --------------------------------------------------------------------------- #
# Region persistence
# --------------------------------------------------------------------------- #

def validate_region(data: dict) -> dict:
    """Coerce/validate a region dict to {x, y, width, height} ints."""
    if not isinstance(data, dict):
        raise ValueError(f"region must be a JSON object, got {type(data).__name__}")
    missing = [k for k in REGION_KEYS if k not in data]
    if missing:
        raise ValueError(f"region is missing keys: {', '.join(missing)}")
    region = {}
    for k in REGION_KEYS:
        try:
            region[k] = int(data[k])
        except (TypeError, ValueError):
            raise ValueError(f"region key '{k}' must be an integer, got {data[k]!r}")
    # x/y may be negative on multi-monitor layouts; width/height must be real.
    if region["width"] <= 0 or region["height"] <= 0:
        raise ValueError(f"region width/height must be positive: {region}")
    return region


def load_region(path: Optional[Path] = None) -> dict:
    path = path or region_file()
    if not path.exists():
        tool = rel(Path(__file__))
        raise FileNotFoundError(
            f"no capture region calibrated yet ({rel(path)}).\n"
            f"  python3 {tool} calibrate                 # drag a rectangle, or\n"
            f"  python3 {tool} calibrate --region X Y W H # set it directly"
        )
    try:
        data = json.loads(path.read_text())
    except json.JSONDecodeError as e:
        raise ValueError(f"region file {rel(path)} is not valid JSON: {e}") from e
    return validate_region(data)


def save_region(region: dict, path: Optional[Path] = None) -> Path:
    path = path or region_file()
    region = validate_region(region)
    path.parent.mkdir(parents=True, exist_ok=True)
    # Pretty-printed so a human can hand-edit it if a drag isn't convenient.
    path.write_text(json.dumps(region, indent=2) + "\n")
    return path


# --------------------------------------------------------------------------- #
# Capture
# --------------------------------------------------------------------------- #

def capture_region(x: int, y: int, width: int, height: int, output: Path) -> Path:
    """Grab one screen region to `output` (PNG). Requires the `mss` package."""
    try:
        import mss
        import mss.tools
    except ImportError as e:
        raise RuntimeError(
            "the `mss` package is required for capture — install it with "
            "`pip install mss` in the environment that owns the display.\n"
            "(Capture cannot work through WSL: there is no host display to grab. "
            "On Windows-via-WSL, take a screenshot manually — see AGENTS.md.)"
        ) from e

    output.parent.mkdir(parents=True, exist_ok=True)
    # mss >= 10 prefers `MSS`; fall back to the legacy `mss()` factory.
    sct_factory = getattr(mss, "MSS", None) or mss.mss
    with sct_factory() as sct:
        monitor = {"left": x, "top": y, "width": width, "height": height}
        shot = sct.grab(monitor)
        mss.tools.to_png(shot.rgb, shot.size, output=str(output))
    return output


# --------------------------------------------------------------------------- #
# Interactive calibration (tkinter drag)
# --------------------------------------------------------------------------- #

def calibrate_interactive() -> dict:
    """Open a translucent fullscreen overlay; user drags a rectangle over the app.

    Returns the region in screen coordinates. Press Escape to cancel.
    Single-monitor assumption: the overlay maps canvas coordinates directly to
    screen coordinates. For multi-monitor setups, prefer
    `calibrate --region X Y W H`.
    """
    try:
        import tkinter as tk
    except ImportError as e:
        raise RuntimeError(
            "interactive calibration needs the standard-library `tkinter`, which "
            "is not available here (on Debian/Ubuntu: `apt install python3-tk`). "
            "Either install it or set the region directly:\n"
            f"  python3 {rel(Path(__file__))} calibrate --region X Y W H"
        ) from e

    result: dict = {}
    root = tk.Tk()
    root.attributes("-fullscreen", True)
    try:
        root.attributes("-alpha", 0.3)  # see the app through the overlay
    except tk.TclError:
        pass  # not all window managers support per-window alpha
    root.attributes("-topmost", True)
    root.configure(bg="black")

    canvas = tk.Canvas(root, cursor="cross", bg="black", highlightthickness=0)
    canvas.pack(fill="both", expand=True)
    canvas.create_text(
        root.winfo_screenwidth() // 2, 30, fill="white",
        font=("TkDefaultFont", 16),
        text="Drag a rectangle around the application window.   Esc to cancel.",
    )

    state = {"sx": 0, "sy": 0, "cx": 0, "cy": 0, "rect": None}

    def on_press(ev):
        state["sx"], state["sy"] = ev.x_root, ev.y_root  # screen coords
        state["cx"], state["cy"] = ev.x, ev.y            # canvas coords
        if state["rect"] is not None:
            canvas.delete(state["rect"])
        state["rect"] = canvas.create_rectangle(
            ev.x, ev.y, ev.x, ev.y, outline="red", width=2,
        )

    def on_drag(ev):
        if state["rect"] is not None:
            canvas.coords(state["rect"], state["cx"], state["cy"], ev.x, ev.y)

    def on_release(ev):
        x0, y0, x1, y1 = state["sx"], state["sy"], ev.x_root, ev.y_root
        result.update(
            x=min(x0, x1), y=min(y0, y1),
            width=abs(x1 - x0), height=abs(y1 - y0),
        )
        root.destroy()

    canvas.bind("<ButtonPress-1>", on_press)
    canvas.bind("<B1-Motion>", on_drag)
    canvas.bind("<ButtonRelease-1>", on_release)
    root.bind("<Escape>", lambda _ev: root.destroy())
    root.mainloop()

    if not result or result.get("width", 0) <= 0 or result.get("height", 0) <= 0:
        raise RuntimeError("calibration cancelled or empty selection — nothing saved.")
    return result


# --------------------------------------------------------------------------- #
# Commands
# --------------------------------------------------------------------------- #

def cmd_calibrate(args) -> int:
    if args.region is not None:
        x, y, w, h = args.region
        region = {"x": x, "y": y, "width": w, "height": h}
        try:
            path = save_region(region)
        except ValueError as e:
            print(f"error: {e}", file=sys.stderr)
            return 2
        print(f"Stored region {region} -> {rel(path)}")
        return 0
    try:
        region = calibrate_interactive()
        path = save_region(region)
    except (RuntimeError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    print(f"Calibrated region {region} -> {rel(path)}")
    return 0


def cmd_capture(args) -> int:
    try:
        region = load_region()
    except (FileNotFoundError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    if args.output:
        out = Path(args.output).expanduser()
    else:
        out = DEFAULT_CAPTURE_DIR / f"capture-{time.strftime('%Y%m%d-%H%M%S')}.png"
    try:
        out = capture_region(region["x"], region["y"], region["width"], region["height"], out)
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    print(f"Captured {region['width']}x{region['height']} at "
          f"({region['x']},{region['y']}) -> {rel(out)}")
    return 0


def cmd_show(args) -> int:
    try:
        region = load_region()
    except (FileNotFoundError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(region))
    else:
        print(f"x={region['x']} y={region['y']} "
              f"width={region['width']} height={region['height']}")
    return 0


def cmd_where(_args) -> int:
    print(rel(region_file()))
    return 0


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="debugviz",
        description="Region-based screen capture for visual debugging of Omega graphics apps.",
    )
    sub = p.add_subparsers(dest="command", required=True)

    c = sub.add_parser("calibrate", help="define the capture region")
    c.add_argument(
        "--region", nargs=4, type=int, metavar=("X", "Y", "W", "H"),
        help="set the region directly (no GUI); use under WSL/headless",
    )
    c.set_defaults(func=cmd_calibrate)

    c = sub.add_parser("capture", help="grab the stored region to a PNG (needs mss)")
    c.add_argument("-o", "--output", help="output PNG path (default: captures/capture-<ts>.png)")
    c.set_defaults(func=cmd_capture)

    c = sub.add_parser("show", help="print the stored region")
    c.add_argument("--json", action="store_true", help="machine-readable output")
    c.set_defaults(func=cmd_show)

    c = sub.add_parser("where", help="print the region-file path")
    c.set_defaults(func=cmd_where)

    return p


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
