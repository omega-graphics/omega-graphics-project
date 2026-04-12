from pathlib import Path
import sys

import pygments.styles


DOCS_DIR = Path(__file__).resolve().parent
REPO_ROOT = DOCS_DIR.parent.parent
sys.path.insert(0, str(REPO_ROOT / "utils" / "sphinx"))

from shared_conf import apply_shared_sphinx_style

project = "OmegaWTK"

author = "Omega Graphics"

copyright = "2021," + author
version = "0.6"

extensions = ["sphinx.ext.viewcode"]

apply_shared_sphinx_style(globals(), DOCS_DIR)
