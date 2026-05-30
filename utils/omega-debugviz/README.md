# omega-debugviz

> ⚠️ **Status: still being tested — do not use for verification yet.** Until this
> tool is signed off, do not rely on it to capture output. When you need to see
> the app, hand off to the user: ask them to take a screenshot and submit it.
> Everything below is documented for when the tool is ready.

Region-based screen capture for **visual debugging** of Omega graphics apps.

Testing a graphics application is not finished until you have *seen* its output.
A green test suite proves the code ran; it does not prove the frame rendered
correctly. `omega-debugviz` lets an agent (or a human) look at the running app
by capturing a single, user-defined rectangular region of the screen and saving
it as a PNG that can then be analyzed.

## Why a fixed region instead of a specific window

The tool deliberately captures a screen *region* rather than chasing a named
window:

- Works identically on Windows, macOS, X11 and Wayland.
- Needs no window-enumeration APIs and hits no Wayland window-discovery
  permission issues.
- Works with games, editors, launchers and custom rendering engines.
- Fast — only a subsection of the screen is read.

The region is stored in screen coordinates, so it must be (re)calibrated
whenever the app moves, the resolution changes, or a laptop is docked/undocked.

## Dependencies

| Dependency | Needed for | Install |
| --- | --- | --- |
| [`mss`](https://pypi.org/project/mss/) | `capture` | `pip install mss` |
| `tkinter` (Python stdlib) | interactive `calibrate` (drag) | usually bundled; on Debian/Ubuntu `apt install python3-tk` |

`mss` must be installed in whatever environment owns the display. `tkinter` is
only needed for the interactive drag — `calibrate --region`, `show` and `where`
work without it. (`region.json` is plain JSON, so you can also hand-edit it.)

> **WSL note:** capture and the drag overlay cannot work through WSL — there is
> no host display to grab. On Windows-via-WSL, set the region with `--region`
> and have the user take a screenshot manually. See the *Visual Debugging*
> section of the repo `AGENTS.md`.

## Usage

```sh
# Calibrate by dragging a rectangle around the app (needs a display + tkinter):
python3 utils/omega-debugviz/debugviz.py calibrate

# ...or set the region directly (WSL/headless, or coordinates already known):
python3 utils/omega-debugviz/debugviz.py calibrate --region 100 80 1600 900

# Grab the stored region to a PNG (default: captures/capture-<timestamp>.png):
python3 utils/omega-debugviz/debugviz.py capture
python3 utils/omega-debugviz/debugviz.py capture -o /tmp/frame.png

# Inspect the current region:
python3 utils/omega-debugviz/debugviz.py show
python3 utils/omega-debugviz/debugviz.py show --json
python3 utils/omega-debugviz/debugviz.py where    # path of the region file
```

## Files

- `debugviz.py` — the tool (single file, stdlib + `mss`).
- `region.json` — the calibrated region `{x, y, width, height}` (git-ignored,
  machine-specific). Overridable via the `OMEGA_DEBUGVIZ_REGION` env var.
- `captures/` — default output directory for screenshots (git-ignored).

## Calibration workflow

Calibrate **after** the app has loaded once, since the region is in screen
coordinates. Keep the app open and recalibrate in place — you should never have
to close and reopen the app just to recalibrate.

The `/calibrate` slash command (`.claude/commands/calibrate.md`), backed by the
`viz-calibrate` hook (`.claude/hooks/viz-calibrate.py`), drives this flow:

```sh
python3 .claude/hooks/viz-calibrate.py                  # interactive drag
python3 .claude/hooks/viz-calibrate.py --region X Y W H # set coordinates directly
```
