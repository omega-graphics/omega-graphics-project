"""Build driver for the OmegaSL VSCode extension.

Converts the canonical OmegaSL grammar at gte/omegasl/syntax/omegasl.yaml
into the VSCode-flavoured TextMate JSON at ide/vscode/syntaxes/, then runs
`npm install` and `npx vsce package` inside ide/vscode/ to produce a .vsix.

Run from anywhere -- paths are resolved relative to this file.
"""

from __future__ import annotations

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import textmate  # noqa: E402  sibling module


REPO_ROOT = os.path.abspath(os.path.join(_HERE, "..", ".."))
GRAMMAR_SRC = os.path.join(REPO_ROOT, "gte", "omegasl", "syntax", "omegasl.yaml")
VSCODE_DIR = os.path.join(REPO_ROOT, "ide", "vscode")
SYNTAXES_DIR = os.path.join(VSCODE_DIR, "syntaxes")
GRAMMAR_OUT = os.path.join(SYNTAXES_DIR, "OmegaSL.tmLanguage.json")


def convert_grammar():
    if not os.path.exists(GRAMMAR_SRC):
        raise SystemExit(f"OmegaSL grammar not found at {GRAMMAR_SRC}")
    os.makedirs(SYNTAXES_DIR, exist_ok=True)
    textmate.TextMateGrammarProcessor().build_grammar(GRAMMAR_SRC, True, GRAMMAR_OUT)
    print(f"Wrote {os.path.relpath(GRAMMAR_OUT, REPO_ROOT)}")


def package_extension():
    cwd = os.getcwd()
    try:
        os.chdir(VSCODE_DIR)
        if os.system("npm install") != 0:
            raise SystemExit("npm install failed")
        if os.system("npx --yes vsce package") != 0:
            raise SystemExit("vsce package failed")
    finally:
        os.chdir(cwd)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    package = "--no-package" not in argv

    convert_grammar()
    if package:
        package_extension()
    else:
        print("Skipping npm install / vsce package (--no-package).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
