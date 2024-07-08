
import pygments.lexers._mapping
import sys
import os

sys.path.append(os.path.dirname(__file__))
import omegasl_highlight

pygments.lexers._mapping.LEXERS["OmegaSLLexer"] = ('omegasl_highlight', 'OmegaSL', ('omegasl',), ('AUTOM.build', '*.build'), ('text/omegasl',))


project = "OmegaGTE"

author = "Omega Graphics"

copyright = "2021," + author
version = "0.3"

extensions = ["sphinx.ext.viewcode"]

# html_theme_path = ["_themes"]
html_theme = "alabaster"



