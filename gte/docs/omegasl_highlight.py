__all__ = [
    "OmegaSLLexer"
]

import pygments
from pygments.lexer import ExtendedRegexLexer, words
from pygments.formatters.terminal256 import TerminalTrueColorFormatter
from pygments import token
import io
import re


class OmegaSLLexer(ExtendedRegexLexer):
    name = 'OmegaSL'
    aliases = ['omegasl']
    filenames = ['*.omegasl']
    tokens = {
        'root': [
            (r'//.*', token.Comment),
            (r'\s+', token.Whitespace),
            (r'\n',token.Whitespace),
            (words(('struct','internal','vertex','fragment','compute','if','else','for','while','return','in','out','inout'), suffix=r'\b'), token.Keyword),
            (words(('void','float','float2','float3','float4','uint','int','buffer','texture1d','texture2d','texture3d'), suffix=r'\b'), token.Keyword.Type),
            (r'\w+', token.Text),
            (r'\(|\)|\[|\]|,|;|:|.',token.Punctuation),
            (words(('=','+=','+','-=','-','*','&','==')),token.Operator)
        ]
    }


if __name__ == "__main__":
    print(pygments.highlight(io.open("../omegasl/tests/shaders.omegasl","r").read(),OmegaSLLexer(),TerminalTrueColorFormatter()))