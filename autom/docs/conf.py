from pathlib import Path
import sys


DOCS_DIR = Path(__file__).resolve().parent
REPO_ROOT = DOCS_DIR.parent.parent
sys.path.append(str(DOCS_DIR / "_ext"))
sys.path.insert(0, str(REPO_ROOT / "utils" / "sphinx"))

from shared_conf import apply_shared_sphinx_style


project = "AUTOM Build System"

author = "Omega Graphics"

copyright = "2021, Omega Graphics"

extensions = ["autom-sphinx"]

highlight_language = "autom"

html_codeblock_linenos_style = 'inline'

apply_shared_sphinx_style(globals(), DOCS_DIR)
