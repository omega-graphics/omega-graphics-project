from pathlib import Path
import sys


DOCS_DIR = Path(__file__).resolve().parent
REPO_ROOT = DOCS_DIR.parent
sys.path.insert(0, str(REPO_ROOT / "utils" / "sphinx"))

from shared_conf import apply_shared_sphinx_style


project = "Omega Graphics Project"

author = "Omega Graphics"

copyright = "2026, " + author
version = "0.1"

extensions = []

exclude_patterns = ["_build"]

html_show_sourcelink = False

apply_shared_sphinx_style(globals(), DOCS_DIR)
