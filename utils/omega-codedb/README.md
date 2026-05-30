# omega-codedb — find your way around the repo

The Omega Graphics tree is large — well over a hundred thousand lines of C++ spread
across half a dozen modules, plus a whole shading language of its own. When you (or
an automated agent) need to answer a question like *"where does the Vulkan backend
live?"* or *"which file defines `GECommandQueue`?"*, searching the whole tree by hand
is slow and turns up a lot of noise.

`omega-codedb` is a small tool that answers those two questions directly. It needs no
installation and no third-party packages: if you have Python 3, you can run it. There
is nothing to build and nothing to set up.

## The idea, in one minute

The tool thinks about the repository in two complementary ways.

**Areas** are named regions of the project — *"OmegaWTK Composition Engine"*, *"OmegaGTE
Vulkan Backend"*, *"OmegaSL Frontend"*, and so on. Each area is a hand-written entry in
the map file `OMEGA-Project.json` that points at one or more folders (and, when several
unrelated pieces share a single folder, at specific files). Areas answer the question
*"where do I go to work on X?"*

**Symbols** are the actual things defined in the code — every C++ class, struct, enum,
union, namespace, and type alias, plus every OmegaSL shader and shader struct. The tool
discovers these automatically by reading the source, and remembers which file and line
each one lives on, and which area it belongs to. Symbols answer the question *"where is
this exact thing defined?"*

You don't have to choose between the two. A typical session starts broad ("which area?")
and narrows to exact ("which file and line?").

## Quick start

All commands are run through the script directly:

```
python3 utils/omega-codedb/codedb.py <command> [options]
```

Find which part of the project owns a topic:

```
$ python3 utils/omega-codedb/codedb.py where "vulkan compositor font"
OmegaGTE Vulkan Backend  (gte/src/vulkan)  [34 symbols]
    Vulkan implementation of the graphics engine for Linux.
    e.g. GEVulkanNativeRenderTarget, GEVulkanTexture, GTEVulkanShader, ...
OmegaWTK Vulkan Backend (Linux)  (wtk/src/Composition/backend/vk)  [12 symbols]
    Vulkan/Harfbuzz implementation of the WTK compositor for Linux (X11/Wayland).
    e.g. HarfBuzzFont, HarfBuzzShaper, HarfBuzzFontEngine, ...
```

Locate a specific type by name — note how it finds the public class *and* every
platform backend that implements it, each labelled with the area it belongs to:

```
$ python3 utils/omega-codedb/codedb.py find CommandQueue
GECommandQueue        class   gte/include/omegaGTE/GECommandQueue.h:118  [OmegaGTE Public API]
GED3D12CommandQueue   class   gte/src/d3d12/GED3D12CommandQueue.h:166    [OmegaGTE Direct3D 12 Backend]
GEMetalCommandQueue   class   gte/src/metal/GEMetalCommandQueue.h:149    [OmegaGTE Metal Backend]
GEVulkanCommandQueue  class   gte/src/vulkan/GEVulkanCommandQueue.h:233  [OmegaGTE Vulkan Backend]
```

That last example is the everyday workhorse: when a change has to land the same way on
all three graphics backends, one lookup shows you every place that needs touching.

## The commands

| Command | What it does |
| --- | --- |
| `where <topic>` | Rank the areas most relevant to a free-text topic. Your starting point when you don't know where something lives. |
| `find <name>` | Locate a symbol (class, struct, enum, namespace, type alias, or OmegaSL shader/struct) by name, with its file, line, and area. |
| `show <area>` | List the files and symbols inside one area. |
| `areas` | List every curated area. Add `--module gte` (or `wtk`, `common`, …) to focus on one module. |
| `stats` | Show how much of the repo is covered, broken down by area and by kind of symbol. Useful for spotting gaps in the map. |
| `index` | Rebuild the symbol index. You rarely need this by hand — every command refreshes automatically — but `index --rebuild` forces a full re-scan. |

A few handy options:

- `--json` — on any command, print the result as JSON instead of text. This is what an
  automated agent should use, because the output is easy to parse.
- `--limit N` — cap how many results `where`, `find`, and `show` print.
- `--kind <prefix>` — on `find`, restrict to one kind of symbol. For example,
  `find reverse --kind osl-shader` returns only shaders whose name contains "reverse",
  and `find Pipeline --kind class` returns only classes.

Matching is forgiving: an exact name beats a name that merely starts with your query,
which beats a word inside a camel-cased name, which beats a plain substring. So you can
type a short fragment and let the ranking float the best hit to the top.

## Keeping the map up to date

The list of areas lives in `OMEGA-Project.json`. It is meant to be edited by hand as the
project grows — think of it as the table of contents for the codebase. Each area is one
entry that looks like this:

```json
{
    "name": "OmegaGTE Vulkan Backend",
    "module": "gte",
    "dirs": ["gte/src/vulkan"],
    "purpose": "Vulkan implementation of the graphics engine for Linux.",
    "tags": ["vulkan", "linux", "backend"]
}
```

- **name** — how the area shows up in results. Make it something a person would recognise.
- **dirs** — one or more folders the area covers. An area often lists both a public-header
  folder and its matching source folder, because they're really one subsystem.
- **purpose** — a sentence describing what lives here and when you'd look. This is shown to
  the reader and also feeds the `where` search, so write it the way someone would ask.
- **tags** — the words a searcher might type. These carry a lot of weight in `where`, so it
  is worth listing synonyms (for example, `direct2d`, `d2d`, `dwrite`, `windows`).
- **key_symbols** / **key_headers** *(optional)* — a short list of the most important types
  and headers to surface for the area.

When several unrelated pieces share a single folder — as the OmegaSL compiler stages all
do inside `gte/omegasl/src` — you can pin an area to specific files instead of a whole
folder by listing `globs`:

```json
{
    "name": "OmegaSL Frontend",
    "module": "gte",
    "globs": ["gte/omegasl/src/Lexer.*", "gte/omegasl/src/Parser.*", "gte/omegasl/src/Sema.*"],
    "purpose": "Where OmegaSL source text becomes a checked syntax tree.",
    "tags": ["frontend", "lexer", "parser", "sema"]
}
```

A file always belongs to the *most specific* area that claims it: a glob beats a folder,
and a deeper folder beats a shallower one. So you can safely have a broad area for a whole
module and narrower areas for the interesting corners inside it — every file lands in the
narrowest one that applies.

After editing the map, run `stats` to check your work. It will warn you if an area points
at a folder that doesn't exist, and it shows how many symbols ended up in each area (and
how many, if any, didn't land in an area at all).

## How it works, briefly

When you run a command, the tool walks the source tree once, reads each C++ and OmegaSL
file, and pulls out the things worth navigating to — the type and namespace definitions,
and the shader entry points. Comments and strings are blanked out first so a keyword that
only appears in a comment is never mistaken for a real definition. Each discovered symbol
is matched to its area, and the result is cached under `.cache/` so the next command is
near-instant. The cache is keyed on each file's size and modification time, so only the
files you've actually changed get re-read. A full cold scan of the whole repository takes
about half a second; a warm lookup is faster than the time it takes Python to start.

The `.cache/` folder is local and disposable — it is ignored by git and can be deleted at
any time; it will simply be rebuilt on the next run.

## What it does and doesn't cover

It indexes the *definitions of types and shaders* — the anchors you navigate **to**. It
deliberately does not try to index every free function, method, or variable: those are far
more numerous and far noisier to detect reliably, and for finding a function by name a
plain text search is usually good enough. If you know the exact symbol you want and it's a
type or a shader, `find` will take you straight there. If you only know the topic, `where`
will point you at the right neighbourhood, and `show` will lay out what's in it.
