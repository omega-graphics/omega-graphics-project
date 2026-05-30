---
description: (Re)calibrate the omega-debugviz screen-capture region for visual debugging
---

The user wants to (re)calibrate the screen region that `omega-debugviz` captures
for visual debugging (see the "Visual Debugging" section of `AGENTS.md`).

Do this:

1. Make sure the graphics app is already running and visible. If it isn't, ask
   the user to launch it first — the region is stored in *screen* coordinates,
   so it only makes sense once the window is on screen. Keep the app open; the
   user should not have to close and reopen it to recalibrate.

2. Run the viz-calibrate hook, which drives `omega-debugviz calibrate`:

   ```sh
   python3 .claude/hooks/viz-calibrate.py
   ```

   This opens a translucent fullscreen overlay — tell the user to drag a
   rectangle around the application window (Esc cancels). The region is saved to
   `utils/omega-debugviz/region.json`.

   If the drag overlay can't run (no display, no `tkinter`, or running through
   WSL), fall back to setting the coordinates directly. If `$ARGUMENTS` already
   contains four numbers, pass them straight through; otherwise ask the user for
   the window's X/Y position and width/height:

   ```sh
   python3 .claude/hooks/viz-calibrate.py --region X Y W H
   ```

3. Confirm the stored region back to the user:

   ```sh
   python3 utils/omega-debugviz/debugviz.py show
   ```

4. Through WSL there is no host display to capture. In that case do not try to
   capture — ask the user to take a screenshot manually and paste it.

$ARGUMENTS
