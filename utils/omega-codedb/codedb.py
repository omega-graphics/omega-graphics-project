#!/usr/bin/env python3
"""omega-codedb — a zero-dependency code locator for the Omega Graphics monorepo.

The repo is ~125k lines of C++ across half a dozen modules plus an embedded
shading language (OmegaSL). Grepping that by hand is slow and noisy. This tool
gives agents (and humans) two complementary ways to find their way around:

  1. Areas   — a hand-curated map of named regions -> directories
               (sourced from OMEGA-Project.json). Answers "where do I go to
               work on X?" at the granularity of a subsystem.

  2. Symbols — an auto-built index of every C++ type (class/struct/enum/union/
               namespace/alias) and every OmegaSL struct/shader entry point,
               with the file:line where it is defined and the area it lives in.
               Answers "where is `GECommandQueue` defined?" exactly.

Everything is standard library only, so it runs anywhere Python 3.8+ runs with
no install step. Output is human-readable by default and machine-readable with
--json so a calling agent can parse it.

Usage examples:
    python3 codedb.py areas                 # list curated areas
    python3 codedb.py where "composition backend"
    python3 codedb.py find GECommandQueue
    python3 codedb.py find compute --kind osl-shader
    python3 codedb.py show "OmegaGTE Source"
    python3 codedb.py stats
    python3 codedb.py index --rebuild       # force a full re-scan
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

# --------------------------------------------------------------------------- #
# Layout / configuration
# --------------------------------------------------------------------------- #

SELF_DIR = Path(__file__).resolve().parent
PROJECT_FILE = SELF_DIR / "OMEGA-Project.json"
CACHE_DIR = SELF_DIR / ".cache"
CACHE_FILE = CACHE_DIR / "symbol-index.json"
CACHE_VERSION = 2  # bump when the on-disk cache schema changes

# Files we treat as indexable source, grouped by how we parse them.
CXX_EXTS = {".h", ".hpp", ".hh", ".hxx", ".inl", ".cpp", ".cc", ".cxx", ".c", ".mm", ".m"}
OSL_EXTS = {".omegasl"}
INDEXABLE_EXTS = CXX_EXTS | OSL_EXTS

# Directories that never contain source we care about.
PRUNE_DIRS = {
    ".git", ".cache", "build", "out", ".automdeps", "deps", "third_party",
    "node_modules", "__pycache__", ".idea", ".vscode",
}


def find_repo_root(start: Path) -> Path:
    """Walk up from `start` until we hit the directory that owns the .git dir.

    Falls back to two levels above this script (utils/omega-codedb -> repo root)
    so the tool still works inside a git worktree export or a tarball.
    """
    cur = start
    for _ in range(64):
        if (cur / ".git").exists():
            return cur
        if cur.parent == cur:
            break
        cur = cur.parent
    return SELF_DIR.parent.parent


REPO_ROOT = find_repo_root(SELF_DIR)


# --------------------------------------------------------------------------- #
# Curated areas (OMEGA-Project.json)
# --------------------------------------------------------------------------- #


# A directory-prefix match is scored by the length of the matched directory, so
# a nested area (wtk/src/Composition/backend/mtl) always beats the enclosing one
# (wtk/src/Composition). A glob match is more specific still: it lets a curator
# pin individual files inside an otherwise-shared directory (the OmegaSL compiler
# stages all live in gte/omegasl/src, for example), so it outranks any dir match.
GLOB_SPECIFICITY = 10_000


@dataclass
class Area:
    """One curated region of the repo. `name` plus at least one of `dirs`/`globs`
    are required; the rest are optional enrichments the project file may supply.

    An area can span several directories (a subsystem's headers + sources), and
    can additionally — or instead — be defined by file globs when distinct
    subsystems share one flat directory."""
    name: str
    dirs: List[str] = field(default_factory=list)
    globs: List[str] = field(default_factory=list)
    module: str = ""
    purpose: str = ""
    tags: List[str] = field(default_factory=list)
    key_headers: List[str] = field(default_factory=list)
    key_symbols: List[str] = field(default_factory=list)

    @property
    def primary_dir(self) -> str:
        if self.dirs:
            return self.dirs[0]
        return self.globs[0] if self.globs else ""

    def exists(self) -> bool:
        for d in self.dirs:
            if (REPO_ROOT / d).is_dir():
                return True
        for g in self.globs:
            if any(REPO_ROOT.glob(g)):
                return True
        return False

    def match_specificity(self, norm_path: str) -> int:
        """How strongly this area claims `norm_path` (-1 == no claim)."""
        best = -1
        for d in self.dirs:
            if norm_path == d or norm_path.startswith(d + "/"):
                best = max(best, len(d))
        for g in self.globs:
            if fnmatch.fnmatchcase(norm_path, g):
                best = max(best, GLOB_SPECIFICITY + len(g))
        return best


def load_areas() -> List[Area]:
    if not PROJECT_FILE.exists():
        die(f"missing project map: {PROJECT_FILE}")
    data = json.loads(PROJECT_FILE.read_text())
    areas: List[Area] = []
    for raw in data.get("partitions", []):
        dirs: List[str] = []
        if raw.get("dir"):
            dirs.append(raw["dir"].rstrip("/"))
        for d in raw.get("dirs", []):
            dirs.append(d.rstrip("/"))
        globs = list(raw.get("globs", []))
        module = raw.get("module", "") or (dirs[0].split("/")[0] if dirs else "")
        areas.append(
            Area(
                name=raw["name"],
                dirs=dirs,
                globs=globs,
                module=module,
                purpose=raw.get("purpose", ""),
                tags=list(raw.get("tags", [])),
                key_headers=list(raw.get("key_headers", [])),
                key_symbols=list(raw.get("key_symbols", [])),
            )
        )
    return areas


def area_for_path(rel_path: str, areas: List[Area]) -> Optional[Area]:
    """Map a repo-relative file path to its single most specific curated area.
    Globs beat directory prefixes; among directories, the longest match wins."""
    norm = rel_path.replace(os.sep, "/")
    best: Optional[Area] = None
    best_spec = -1
    for area in areas:
        spec = area.match_specificity(norm)
        if spec > best_spec:
            best, best_spec = area, spec
    return best if best_spec >= 0 else None


# --------------------------------------------------------------------------- #
# Symbol extraction
# --------------------------------------------------------------------------- #


@dataclass
class Symbol:
    name: str
    kind: str          # namespace|class|struct|union|enum|using|typedef|osl-struct|osl-shader:<stage>
    file: str          # repo-relative
    line: int
    area: str = ""     # filled in after indexing


# --- comment / literal scrubbing ------------------------------------------- #
# We replace comments and string/char literals with same-length whitespace so
# byte offsets (and therefore line numbers) are preserved while we never match
# a keyword that only appears inside a comment or a string.

_SCRUB_RE = re.compile(
    r'//[^\n]*'                       # line comment
    r'|/\*.*?\*/'                     # block comment
    r'|"(?:\\.|[^"\\])*"'             # string literal
    r"|'(?:\\.|[^'\\])*'",            # char literal
    re.DOTALL,
)


def _blank(match: "re.Match[str]") -> str:
    # Keep newlines so line numbers stay correct; blank everything else.
    return "".join("\n" if c == "\n" else " " for c in match.group(0))


def scrub(text: str) -> str:
    return _SCRUB_RE.sub(_blank, text)


def line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


# --- C/C++ ----------------------------------------------------------------- #
# Optional all-caps attribute macros (OMEGAGTE_EXPORT, OMEGACOMMON_NODISCARD,
# ...) may sit between the keyword and the type name; we skip over them.
_ATTR = r'(?:[A-Z_][A-Z0-9_]*\s+)*'

CXX_TYPE_RE = re.compile(
    r'\b(class|struct|union|enum)\b'
    r'(?:\s+class|\s+struct)?'                 # enum class / enum struct
    r'\s+' + _ATTR +
    r'([A-Za-z_]\w*)'                          # 2: name
    r'(?:\s+final)?'
    r'\s*(?:: *[^;{}]*)?'                      # optional base-clause
    r'\s*\{',                                  # must be a definition, not a fwd decl
)

CXX_NS_RE = re.compile(r'\bnamespace\s+([A-Za-z_][\w]*(?:\s*::\s*[A-Za-z_]\w*)*)\s*\{')
CXX_USING_RE = re.compile(r'\busing\s+([A-Za-z_]\w*)\s*=')
CXX_TYPEDEF_RE = re.compile(r'\btypedef\b[^;{}]*?\b([A-Za-z_]\w*)\s*;')


def extract_cxx(text: str) -> List[Tuple[str, str, int]]:
    out: List[Tuple[str, str, int]] = []
    clean = scrub(text)
    for m in CXX_TYPE_RE.finditer(clean):
        kw = m.group(1)
        out.append((m.group(2), kw, line_of(clean, m.start())))
    for m in CXX_NS_RE.finditer(clean):
        name = re.sub(r'\s*::\s*', "::", m.group(1))
        out.append((name, "namespace", line_of(clean, m.start())))
    for m in CXX_USING_RE.finditer(clean):
        out.append((m.group(1), "using", line_of(clean, m.start())))
    for m in CXX_TYPEDEF_RE.finditer(clean):
        out.append((m.group(1), "typedef", line_of(clean, m.start())))
    return out


# --- OmegaSL --------------------------------------------------------------- #
OSL_STRUCT_RE = re.compile(r'\bstruct\s+([A-Za-z_]\w*)')
OSL_SHADER_RE = re.compile(
    r'\b(vertex|fragment|compute|mesh|task)\b'
    r'\s*(?:\([^)]*\))?'                       # optional (x=..,y=..) launch dims
    r'\s+(?:[A-Za-z_][\w:<>,&* ]*?\s+)?'       # optional return type
    r'([A-Za-z_]\w*)\s*\(',                    # 2: entry-point name
)


def extract_osl(text: str) -> List[Tuple[str, str, int]]:
    out: List[Tuple[str, str, int]] = []
    clean = scrub(text)
    for m in OSL_STRUCT_RE.finditer(clean):
        out.append((m.group(1), "osl-struct", line_of(clean, m.start())))
    for m in OSL_SHADER_RE.finditer(clean):
        stage = m.group(1)
        out.append((m.group(2), f"osl-shader:{stage}", line_of(clean, m.start())))
    return out


def extract_file(path: Path, ext: str) -> List[Tuple[str, str, int]]:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    if ext in OSL_EXTS:
        return extract_osl(text)
    return extract_cxx(text)


# --------------------------------------------------------------------------- #
# Index build + cache
# --------------------------------------------------------------------------- #


@dataclass
class Index:
    symbols: List[Symbol]
    files: List[str]
    built_at: float


def iter_source_files() -> Iterable[Path]:
    for dirpath, dirnames, filenames in os.walk(REPO_ROOT):
        dirnames[:] = [d for d in dirnames if d not in PRUNE_DIRS and not d.startswith(".")]
        for fn in filenames:
            if os.path.splitext(fn)[1].lower() in INDEXABLE_EXTS:
                yield Path(dirpath) / fn


def _load_cache() -> Optional[dict]:
    if not CACHE_FILE.exists():
        return None
    try:
        data = json.loads(CACHE_FILE.read_text())
    except (OSError, ValueError):
        return None
    if data.get("version") != CACHE_VERSION:
        return None
    return data


def _save_cache(per_file: Dict[str, dict]) -> None:
    CACHE_DIR.mkdir(exist_ok=True)
    payload = {
        "version": CACHE_VERSION,
        "root": str(REPO_ROOT),
        "files": per_file,
    }
    tmp = CACHE_FILE.with_suffix(".tmp")
    tmp.write_text(json.dumps(payload))
    tmp.replace(CACHE_FILE)


def build_index(rebuild: bool = False, quiet: bool = True) -> Index:
    """Incrementally (re)build the symbol index.

    The cache stores, per file, its (mtime, size) and extracted symbols. On each
    run we stat every source file and only re-parse the ones that changed, which
    keeps warm queries effectively instant while staying correct after edits.
    """
    cached = None if rebuild else _load_cache()
    cached_files: Dict[str, dict] = (cached or {}).get("files", {}) if cached else {}

    per_file: Dict[str, dict] = {}
    reparsed = 0
    for path in iter_source_files():
        rel = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
        try:
            st = path.stat()
        except OSError:
            continue
        sig = [int(st.st_mtime), st.st_size]
        prev = cached_files.get(rel)
        if prev and prev.get("sig") == sig:
            per_file[rel] = prev
            continue
        ext = os.path.splitext(path)[1].lower()
        syms = [[n, k, ln] for (n, k, ln) in extract_file(path, ext)]
        per_file[rel] = {"sig": sig, "syms": syms}
        reparsed += 1

    _save_cache(per_file)
    if not quiet:
        eprint(f"indexed {len(per_file)} files ({reparsed} re-parsed)")

    areas = load_areas()
    symbols: List[Symbol] = []
    for rel, info in per_file.items():
        area = area_for_path(rel, areas)
        area_name = area.name if area else "(unpartitioned)"
        for name, kind, line in info["syms"]:
            symbols.append(Symbol(name=name, kind=kind, file=rel, line=line, area=area_name))
    return Index(symbols=symbols, files=sorted(per_file.keys()), built_at=time.time())


# --------------------------------------------------------------------------- #
# Ranking helpers
# --------------------------------------------------------------------------- #

_TOKEN_RE = re.compile(r"[A-Za-z0-9]+")


def tokens(s: str) -> List[str]:
    return [t.lower() for t in _TOKEN_RE.findall(s)]


def score_area(query: str, area: Area, sym_count: int) -> float:
    """Heuristic relevance of an area to a free-text query. Name and tag hits
    weigh most; directory, purpose, and indexed-symbol hits fill in."""
    q = query.lower().strip()
    qtok = set(tokens(query))
    if not qtok:
        return 0.0
    score = 0.0
    name_l = area.name.lower()
    if q in name_l:
        score += 12.0
    ntok = set(tokens(area.name))
    score += 6.0 * len(qtok & ntok)
    score += 5.0 * len(qtok & set(t.lower() for t in area.tags))
    score += 3.0 * len(qtok & set(tokens(" ".join(area.dirs))))
    if area.purpose:
        score += 1.5 * len(qtok & set(tokens(area.purpose)))
    score += 2.0 * len(qtok & set(tokens(" ".join(area.key_symbols))))
    return score


def score_symbol(query: str, sym: Symbol) -> float:
    """Rank a symbol against a name query: exact > prefix > word > substring."""
    q = query.lower()
    n = sym.name.lower()
    if n == q:
        return 100.0
    if n.startswith(q):
        return 60.0 - min(len(n) - len(q), 30)
    if q in tokens_camel(sym.name):
        return 40.0
    if q in n:
        return 20.0 - min(n.index(q), 15)
    return 0.0


_CAMEL_RE = re.compile(r"[A-Z]+(?![a-z])|[A-Z][a-z0-9]*|[a-z0-9]+")


def tokens_camel(name: str) -> List[str]:
    return [t.lower() for t in _CAMEL_RE.findall(name)]


# --------------------------------------------------------------------------- #
# Output helpers
# --------------------------------------------------------------------------- #


def eprint(*a) -> None:
    print(*a, file=sys.stderr)


def die(msg: str) -> "None":
    eprint(f"omega-codedb: error: {msg}")
    raise SystemExit(2)


def emit(obj, as_json: bool, human) -> None:
    if as_json:
        print(json.dumps(obj, indent=2))
    else:
        human(obj)


# --------------------------------------------------------------------------- #
# Commands
# --------------------------------------------------------------------------- #


def cmd_areas(args) -> None:
    areas = load_areas()
    if args.module:
        m = args.module.lower()
        areas = [a for a in areas if a.module.lower() == m or m in a.name.lower()]
    rows = []
    for a in areas:
        loc = a.primary_dir + (f"  (+{len(a.dirs) - 1} more)" if len(a.dirs) > 1 else "")
        rows.append({
            "name": a.name,
            "location": loc,
            "dirs": a.dirs,
            "globs": a.globs,
            "module": a.module,
            "exists": a.exists(),
            "purpose": a.purpose,
            "tags": a.tags,
        })

    def human(rs):
        if not rs:
            eprint("(no areas)")
            return
        w = max(len(r["name"]) for r in rs)
        for r in rs:
            flag = "" if r["exists"] else "  [MISSING]"
            print(f"{r['name']:<{w}}  {r['location']}{flag}")
            if r["purpose"]:
                print(f"{'':<{w}}    {r['purpose']}")

    emit(rows, args.json, human)


def cmd_where(args) -> None:
    areas = load_areas()
    idx = build_index(quiet=True)
    counts: Dict[str, int] = {}
    for s in idx.symbols:
        counts[s.area] = counts.get(s.area, 0) + 1

    scored = []
    for a in areas:
        sc = score_area(args.query, a, counts.get(a.name, 0))
        if sc > 0:
            scored.append((sc, a))
    scored.sort(key=lambda t: (-t[0], t[1].name))
    scored = scored[: args.limit]

    results = []
    for sc, a in scored:
        sample: List[str] = []
        for s in idx.symbols:
            if s.area == a.name and s.name not in sample:
                sample.append(s.name)
            if len(sample) >= 6:
                break
        results.append({
            "name": a.name,
            "location": a.primary_dir,
            "dirs": a.dirs,
            "globs": a.globs,
            "score": round(sc, 1),
            "purpose": a.purpose,
            "symbol_count": counts.get(a.name, 0),
            "sample_symbols": sample,
        })

    def human(rs):
        if not rs:
            eprint(f"no areas matched {args.query!r}")
            return
        for r in rs:
            print(f"{r['name']}  ({r['location']})  [{r['symbol_count']} symbols]")
            if r["purpose"]:
                print(f"    {r['purpose']}")
            if r["sample_symbols"]:
                print(f"    e.g. {', '.join(r['sample_symbols'])}")

    emit(results, args.json, human)


def cmd_find(args) -> None:
    if not args.query.strip():
        die("find needs a non-empty symbol query")
    idx = build_index(quiet=True)
    matches = []
    for s in idx.symbols:
        if args.kind and not s.kind.startswith(args.kind):
            continue
        sc = score_symbol(args.query, s)
        if sc > 0:
            matches.append((sc, s))
    matches.sort(key=lambda t: (-t[0], t[1].file, t[1].line))
    matches = matches[: args.limit]

    results = [
        {"name": s.name, "kind": s.kind, "file": s.file, "line": s.line, "area": s.area}
        for _, s in matches
    ]

    def human(rs):
        if not rs:
            eprint(f"no symbol matched {args.query!r}")
            return
        w = max(len(r["name"]) for r in rs)
        for r in rs:
            print(f"{r['name']:<{w}}  {r['kind']:<16}  {r['file']}:{r['line']}  [{r['area']}]")

    emit(results, args.json, human)


def cmd_show(args) -> None:
    areas = load_areas()
    target = None
    q = args.area.lower()
    for a in areas:
        if a.name.lower() == q or q in [d.lower() for d in a.dirs]:
            target = a
            break
    if target is None:
        cands = [a for a in areas if q in a.name.lower() or any(q in d.lower() for d in a.dirs)]
        if len(cands) == 1:
            target = cands[0]
        elif len(cands) > 1:
            die("ambiguous area; matches: " + ", ".join(a.name for a in cands))
        else:
            die(f"no area named {args.area!r}")

    idx = build_index(quiet=True)
    syms = [s for s in idx.symbols if s.area == target.name]
    files = sorted({s.file for s in syms})
    by_kind: Dict[str, int] = {}
    for s in syms:
        by_kind[s.kind] = by_kind.get(s.kind, 0) + 1

    result = {
        "name": target.name,
        "dirs": target.dirs,
        "globs": target.globs,
        "purpose": target.purpose,
        "file_count": len(files),
        "symbol_count": len(syms),
        "by_kind": by_kind,
        "files": files,
        "symbols": [{"name": s.name, "kind": s.kind, "file": s.file, "line": s.line} for s in syms],
    }

    def human(r):
        loc = ", ".join(r["dirs"] + r["globs"])
        print(f"{r['name']}  ({loc})")
        if r["purpose"]:
            print(f"  {r['purpose']}")
        print(f"  {r['file_count']} files, {r['symbol_count']} symbols  "
              f"({', '.join(f'{k}:{v}' for k, v in sorted(r['by_kind'].items()))})")
        for s in r["symbols"][: args.limit]:
            print(f"    {s['name']:<28} {s['kind']:<14} {s['file']}:{s['line']}")
        if r["symbol_count"] > args.limit:
            print(f"    ... and {r['symbol_count'] - args.limit} more (use --json for all)")

    emit(result, args.json, human)


def cmd_stats(args) -> None:
    areas = load_areas()
    idx = build_index(quiet=True)
    by_area: Dict[str, int] = {}
    by_kind: Dict[str, int] = {}
    for s in idx.symbols:
        by_area[s.area] = by_area.get(s.area, 0) + 1
        base = s.kind.split(":")[0]
        by_kind[base] = by_kind.get(base, 0) + 1
    result = {
        "repo_root": str(REPO_ROOT),
        "areas_defined": len(areas),
        "areas_missing": [a.name for a in areas if not a.exists()],
        "files_indexed": len(idx.files),
        "symbols_indexed": len(idx.symbols),
        "symbols_by_kind": by_kind,
        "symbols_by_area": by_area,
    }

    def human(r):
        print(f"repo: {r['repo_root']}")
        print(f"areas defined: {r['areas_defined']}  files indexed: {r['files_indexed']}  "
              f"symbols: {r['symbols_indexed']}")
        if r["areas_missing"]:
            print(f"MISSING area dirs: {', '.join(r['areas_missing'])}")
        print("by kind: " + ", ".join(f"{k}={v}" for k, v in sorted(r["symbols_by_kind"].items())))
        print("by area:")
        for name, n in sorted(r["symbols_by_area"].items(), key=lambda kv: -kv[1]):
            print(f"    {n:>6}  {name}")

    emit(result, args.json, human)


def cmd_index(args) -> None:
    t0 = time.time()
    idx = build_index(rebuild=args.rebuild, quiet=False)
    dt = time.time() - t0
    eprint(f"{len(idx.symbols)} symbols across {len(idx.files)} files in {dt:.2f}s")


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="omega-codedb", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--json", action="store_true", help="emit machine-readable JSON")

    # --json is also accepted *after* the subcommand (git-style), which is what
    # callers reach for first. SUPPRESS keeps an omitted flag from clobbering a
    # value already set at the top level.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--json", action="store_true", default=argparse.SUPPRESS,
                        help="emit machine-readable JSON")

    sub = p.add_subparsers(dest="command", required=True)

    a = sub.add_parser("areas", parents=[common], help="list curated areas (regions of the repo)")
    a.add_argument("--module", help="filter to one top-level module (wtk, gte, ...)")
    a.set_defaults(func=cmd_areas)

    w = sub.add_parser("where", parents=[common], help="find which area(s) a topic lives in")
    w.add_argument("query")
    w.add_argument("--limit", type=int, default=8)
    w.set_defaults(func=cmd_where)

    f = sub.add_parser("find", parents=[common], help="locate a symbol (class/struct/shader/...) by name")
    f.add_argument("query")
    f.add_argument("--kind", help="restrict to a kind prefix (class, struct, osl-shader, ...)")
    f.add_argument("--limit", type=int, default=25)
    f.set_defaults(func=cmd_find)

    s = sub.add_parser("show", parents=[common], help="list files + symbols in one area")
    s.add_argument("area")
    s.add_argument("--limit", type=int, default=40)
    s.set_defaults(func=cmd_show)

    st = sub.add_parser("stats", parents=[common], help="index coverage + curation health")
    st.set_defaults(func=cmd_stats)

    i = sub.add_parser("index", parents=[common], help="(re)build the symbol index cache")
    i.add_argument("--rebuild", action="store_true", help="ignore cache, full re-scan")
    i.set_defaults(func=cmd_index)

    return p


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    args.func(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
