from pathlib import Path
import pygments.lexers._mapping
import sys


DOCS_DIR = Path(__file__).resolve().parent
REPO_ROOT = DOCS_DIR.parent.parent
sys.path.append(str(DOCS_DIR))
sys.path.insert(0, str(REPO_ROOT / "utils" / "sphinx"))

from shared_conf import apply_shared_sphinx_style

import omegasl_highlight

pygments.lexers._mapping.LEXERS["OmegaSLLexer"] = ('omegasl_highlight', 'OmegaSL', ('omegasl',), ('AUTOM.build', '*.build'), ('text/omegasl',))


project = "OmegaGTE"

author = "Omega Graphics"

copyright = "2026," + author
version = "0.3"

extensions = ["sphinx.ext.viewcode"]

apply_shared_sphinx_style(globals(), DOCS_DIR)
