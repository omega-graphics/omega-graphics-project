from __future__ import annotations

import sys
from pathlib import Path

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from autom_deps.cli import parse_args
    from autom_deps.errors import AutomDepsError
    from autom_deps.runner import run
    from autom_deps.ui import emit_error
else:
    from .cli import parse_args
    from .errors import AutomDepsError
    from .runner import run
    from .ui import emit_error


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        return run(argv, args)
    except AutomDepsError as ex:
        emit_error(str(ex), json_log=args.json_log, stream=sys.stderr)
        return 1
    except KeyboardInterrupt:
        emit_error("interrupted", json_log=args.json_log, stream=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
