# AGENTS (Must read before proceeding)

## Code Style
    LLVM/Clang and Ninja for building on all platforms
        Use clang-format, clang-tidy to help cleanup and tidy the project files.
    Use PascalCase for file-naming conventions.
        -Static methods use PascalCase.
    C++ 17 is the default cpp standard of this entire repo
    Use an object-oriented, Modular coding style that has modular rules so if a specific rule needs to be changed across the object it can be.

# Building
    The toolchain is LLVM/Clang + Ninja on every platform (see Code Style), built out-of-source. On this Linux host the agent builds and runs the native Vulkan target directly.

    **Windows builds go through WSL, and the agent cannot drive that build itself.** When a change has to be compiled for Windows, hand the build off to the user: ask them to build under their Windows, then wait for the compiler/linker output and iterate on the errors they paste back. Do not assume a Windows build passed, and do not invent errors you cannot see — work only from what the user reports. (This is the same WSL constraint that forces Visual Debugging to fall back to user-supplied screenshots.)

# Navigating the Codebase
    **Hard preference: omega-codedb over `find` / `grep -r`.** Raw `find`/`grep` over a 100k-line tree is slow, returns noise, and bypasses the curated area map. Default to codedb for every symbol lookup. Only fall back to `grep` when the target is a non-declaration string (literal, error message, comment phrase) that codedb's symbol index does not capture; even then, scope `grep` to the file or directory codedb already pointed you at — never `grep -r` over the whole repo. The find/grep approach takes too long; codedb finishes in under a second.

    This repo is 100k+ lines across many modules. Before grepping the whole tree, use omega-codedb to locate the right area, file, or symbol. It is zero-dependency Python (stdlib only) and needs no build.
        python3 utils/omega-codedb/codedb.py where "<topic>"   # which area owns a topic (e.g. "vulkan compositor")
        python3 utils/omega-codedb/codedb.py find <Symbol>     # exact file:line + area for a class/struct/enum/namespace or OmegaSL shader
        python3 utils/omega-codedb/codedb.py show "<Area>"     # files + symbols inside one area
        python3 utils/omega-codedb/codedb.py areas [--module gte]
        python3 utils/omega-codedb/codedb.py index --rebuild   # refresh the symbol index after adding/moving files
    `find` returns the public type AND its per-backend siblings (D3D12/Metal/Vulkan), so it is the fastest way to scope a change that must land uniformly across backends. Add --json to any command for machine-readable output.
    The symbol index is an auto-built cache; if `find`/`where` look stale after you add or rename types, run `index --rebuild` before trusting the result.
    The curated area map is utils/omega-codedb/OMEGA-Project.json — keep it current when you add or move a subsystem. Full guide: utils/omega-codedb/README.md.
    Typical loop: `where` to pick the area → `find` to get the exact file:line → open that file in chunks (see Reading Files) rather than grepping the whole tree.

## Reading Files:
    Many files in this repo are large (1000+ lines). Always read files in chunks of fewer than 500 lines — never read a whole large file in a single pass. Check the line count first (`wc -l <file>`), then page through it in <500-line ranges so nothing is silently truncated and every region actually gets seen.

    Before making any conclusions:

    1. Read the file (in <500-line chunks).
    2. Quote the exact function signatures you found.
    3. List the classes that actually exist.
    4. If something isn't visible in the file, say "not found".
    5. Do not infer or guess.

## Verification Pass:

Now verify every statement you made against the file.
List anything that was inferred rather than directly observed.

# Dependencies
    All dependencies in this project are pulled in via AUTOMDEPS. (under autom/tools/autom-deps) We often pull the source repos directly and build them allowing us to control how third party deps are shipped with our APIs. However AUTOMDEPS is used to get external tools (Strawberry Perl on Windows) or Archives (VulkanSDK on Linux). If a dependency is missing from a repo tree, rerun autom-deps. (autom/tools/autom-deps), and it will automatically fetch or sync any missing/outdated deps.
# Code Authoring
    All implementations will be revised through multi-phase plans before being implemented.
    (This is only for code, not for documentation, or other utilities)
   - 1 research existing/working ideas solutions from other projects
   - 2 devise a new solution from those old solutions
   - 3 refine to include specific details about features/functionality
   - 4 write the multi-phase plan
   - 5 Implement Incrementally.

    Exception — tests. Test authoring is simple and usually does not warrant the full multi-phase plan. Locate the existing test conventions for the area (file layout, build wiring, helpers), match them, write the test, and move on. Reserve the multi-phase plan for the production code paths the tests exercise.

    When a plan doc already exists. Many production features in this repo are tracked by a dedicated plan doc (e.g. `gte/docs/Mesh-Shader-Implementation-Plan.md`, `gte/docs/Raytracing-Full-Implementation-Plan.md`, `gte/docs/Pipeline-Completion-Extension-Plan.md`). Do NOT write a parallel plan in chat — read the existing one first. If the sub-phase you are about to implement already has its own breakdown there, follow it. If it does NOT, ADD the implementation phasing to the existing plan doc (under the matching Phase section), then implement against your own addition. The plan doc is the source of truth; the chat is the working surface.

# Debugging
    It could either be a bug in the code or an architectural design flaw. If the code logic looks correct but fails to fix the issue, consider a design change to the system based on thorough, grounded research. (For a larger scoped issue that isn't resolving easily, the whole architecture of that region may need to be changed. However some bugs are very subtle, and are only due to the misuse of certain API's. For this, research thoroughly and propose the cleanest patch.)

## Visual Debugging
Testing a graphics application is not finished until you have *seen* its output. A passing test suite proves the code ran; it does not prove the frame rendered correctly. For any GUI or rendering change, verifying the visual result is part of the task, not an optional extra.

> **Status — do not use the tool yet.** `omega-debugviz` (below) is still being tested and is not trusted for verification. For now, on **every** platform, do not run it to gather output: when you need to see the app, hand off to the user — ask them to take a screenshot and submit it, then analyze that image. The tool is documented below for reference until it is signed off.

The screenshot tool is `omega-debugviz` (`utils/omega-debugviz/debugviz.py`) — a Python 3 utility built on `mss`. Instead of enumerating windows, it captures one user-defined rectangular region of the screen:

    python3 utils/omega-debugviz/debugviz.py calibrate                  # drag a rectangle around the app
    python3 utils/omega-debugviz/debugviz.py calibrate --region X Y W H  # or set it directly (WSL/headless)
    python3 utils/omega-debugviz/debugviz.py capture [-o out.png]        # grab the region to a PNG
    python3 utils/omega-debugviz/debugviz.py show                        # print the stored region

Dependencies: `capture` needs the `mss` package (`pip install mss`) in whatever environment owns the display; interactive `calibrate` also needs `tkinter` (Python stdlib; on Debian/Ubuntu `apt install python3-tk`). Neither is needed for `--region`/`show`. Full guide: `utils/omega-debugviz/README.md`.

**This works on native Linux (X11/Wayland), macOS, and native Windows. It does NOT work through WSL** — there is no host display for the tool to capture. When the app is built or run on Windows via WSL (see Building), the agent cannot screenshot it: the user must take a screenshot manually and submit it, and the agent analyzes that image instead.

### Capture region
The user defines the region once by dragging a rectangle around the area where the application appears. Store it as JSON:

    {
      "x": 100,
      "y": 80,
      "width": 1600,
      "height": 900
    }

Every capture is then `capture_region(x, y, width, height)`. This region is the authoritative view of the application under test — after each significant GUI interaction, capture it and analyze the result. Window enumeration and platform-specific capture APIs are secondary fallbacks only.

Capturing a fixed region rather than a specific window is deliberate:
- Works identically on Windows, macOS, X11, and Wayland.
- Needs no window-enumeration APIs and avoids Wayland window-discovery permission issues.
- Works with games, editors, tools, launchers, and custom rendering engines.
- Fast — only a subsection of the screen is read.

### Calibration
The region is in screen coordinates, so calibrate after the app has loaded once. Keep the app open and ask the user to recalibrate in place — they should never have to close and reopen the app just to recalibrate.

`/calibrate` (backed by the `viz-calibrate` hook, `.claude/hooks/viz-calibrate.py`) asks the user to draw a new rectangle and updates the stored coordinates. Recalibration keeps the workflow alive across monitor changes, resolution changes, docking or undocking a laptop, or moving the app to another display — without touching any automation code. For a long-running agent this is far more robust than trying to identify windows automatically.



# Documentation
    All language written in any form of documentation (code comments, sphinx docs, READMEs) must be human readable.
- Code Comments (Doxygen):
    Use accurate, technical language to describe the code details fully and correctly.
- User Guides (Sphinx docs) and README's:
    Use long-form prose that is clear and can be understood by someone with little to no technical experience. (The guides should be easy to follow and be able to explain how the code works without getting too technical.)
    Be sure to double check actual code examples so that any written code samples are accurate to the real API.
