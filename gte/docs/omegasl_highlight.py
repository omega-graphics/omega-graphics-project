__all__ = ["OmegaSLLexer"]

from pathlib import Path

import pygments
from pygments import token
from pygments.formatters.terminal256 import TerminalTrueColorFormatter
from pygments.lexer import RegexLexer, bygroups


IDENT = r"[A-Za-z_][A-Za-z0-9_]*"

BUILTIN_MATH = (
    "sin",
    "cos",
    "tan",
    "asin",
    "acos",
    "atan",
    "atan2",
    "sqrt",
    "abs",
    "floor",
    "ceil",
    "round",
    "frac",
    "exp",
    "exp2",
    "log",
    "log2",
    "normalize",
    "length",
    "pow",
    "min",
    "max",
    "step",
    "reflect",
    "clamp",
    "lerp",
    "smoothstep",
)

BUILTIN_CTOR_RE = (
    r"make_(?:float(?:2|3|4|2x2|2x3|2x4|3x2|3x3|3x4|4x2|4x3|4x4)"
    r"|int(?:2|3|4)|uint(?:2|3|4))"
)
BUILTIN_CORE = ("sample", "read", "write", "dot", "cross")

STAGE_KEYWORDS = ("vertex", "fragment", "compute", "hull", "domain")
CONTROL_KEYWORDS = ("if", "else", "for", "while", "return")
DECLARATION_KEYWORDS = ("struct", "internal", "static")
RESOURCE_ACCESS_KEYWORDS = ("inout", "in", "out")

SCALAR_TYPES = ("void", "bool", "int", "uint", "float", "double")
RESOURCE_TYPES = (
    "buffer",
    "texture1d",
    "texture2d",
    "texture3d",
    "sampler1d",
    "sampler2d",
    "sampler3d",
)
ATTRIBUTE_NAMES = (
    "VertexID",
    "InstanceID",
    "Position",
    "Color",
    "TexCoord",
    "GlobalThreadID",
    "LocalThreadID",
    "ThreadGroupID",
)
DECORATOR_PROPERTIES = (
    "domain",
    "partitioning",
    "outputtopology",
    "outputcontrolpoints",
    "filter",
    "address_mode",
    "max_anisotropy",
    "x",
    "y",
    "z",
)
DECORATOR_VALUES = (
    "linear",
    "point",
    "anisotropic",
    "wrap",
    "clamp_to_edge",
    "mirror",
    "mirror_wrap",
    "tri",
    "quad",
    "integer",
    "fractional_even",
    "fractional_odd",
    "triangle_cw",
    "triangle_ccw",
    "line",
)

VECTOR_TYPES_RE = r"(?:int|uint|float|double)(?:2|3|4)"
MATRIX_TYPES_RE = r"float(?:2x2|2x3|2x4|3x2|3x3|3x4|4x2|4x3|4x4)"
SCALAR_TYPES_RE = "|".join(SCALAR_TYPES)
PREPROCESSOR_DIRECTIVES_RE = r"(?:define|ifdef|ifndef|endif|include)"
FLOAT_LITERAL_RE = r"(?<![A-Za-z0-9_])(?:\d+\.\d*|\.\d+)(?:[eE][+\-]?\d+)?f?\b"
EXP_FLOAT_LITERAL_RE = r"(?<![A-Za-z0-9_])\d+[eE][+\-]?\d+f?\b"
INTEGER_LITERAL_RE = r"(?<![A-Za-z0-9_])\d+u?\b"


class OmegaSLLexer(RegexLexer):
    name = "OmegaSL"
    aliases = ["omegasl"]
    filenames = ["*.omegasl"]

    tokens = {
        "root": [
            (
                rf"^(\s*)(#)(\s*)({PREPROCESSOR_DIRECTIVES_RE})\b",
                bygroups(
                    token.Whitespace,
                    token.Comment.Preproc,
                    token.Whitespace,
                    token.Comment.Preproc,
                ),
                "preprocessor",
            ),
            (r"\s+", token.Whitespace),
            (r"//.*$", token.Comment.Single),
            (r"/\*", token.Comment.Multiline, "comment"),
            (r'"', token.String.Double, "string"),
            (FLOAT_LITERAL_RE, token.Number.Float),
            (EXP_FLOAT_LITERAL_RE, token.Number.Float),
            (INTEGER_LITERAL_RE, token.Number.Integer),
            (
                rf"\b(struct)(\s+)({IDENT})(\s+)(internal)\b",
                bygroups(
                    token.Keyword.Declaration,
                    token.Whitespace,
                    token.Name.Class,
                    token.Whitespace,
                    token.Keyword.Declaration,
                ),
            ),
            (
                rf"\b(struct)(\s+)({IDENT})\b",
                bygroups(token.Keyword.Declaration, token.Whitespace, token.Name.Class),
            ),
            (
                rf"\b(buffer)(\s*)(<)(\s*)({IDENT})(\s*)(>)(\s+)({IDENT})(\s*)(:)(\s*)(\d+)\b",
                bygroups(
                    token.Keyword.Type,
                    token.Whitespace,
                    token.Punctuation,
                    token.Whitespace,
                    token.Name.Class,
                    token.Whitespace,
                    token.Punctuation,
                    token.Whitespace,
                    token.Name.Variable,
                    token.Whitespace,
                    token.Punctuation,
                    token.Whitespace,
                    token.Number.Integer,
                ),
            ),
            (
                rf"\b(texture1d|texture2d|texture3d|sampler1d|sampler2d|sampler3d)\b(\s+)({IDENT})(\s*)(:)?(\s*)(\d+)?",
                bygroups(
                    token.Keyword.Type,
                    token.Whitespace,
                    token.Name.Variable,
                    token.Whitespace,
                    token.Punctuation,
                    token.Whitespace,
                    token.Number.Integer,
                ),
            ),
            (
                rf"(:)(\s*)({'|'.join(ATTRIBUTE_NAMES)})\b",
                bygroups(token.Punctuation, token.Whitespace, token.Name.Attribute),
            ),
            (rf"\[(?=[^\]]*\b(?:{'|'.join(RESOURCE_ACCESS_KEYWORDS)})\b)", token.Punctuation, "resource-map"),
            (
                rf"\b(?:{'|'.join(DECORATOR_PROPERTIES)})\b(?=\s*=)",
                token.Name.Decorator,
            ),
            (rf"\b(?:{'|'.join(DECORATOR_VALUES)})\b", token.Name.Constant),
            (rf"\b(?:{'|'.join(CONTROL_KEYWORDS)})\b", token.Keyword),
            (rf"\b(?:{'|'.join(DECLARATION_KEYWORDS)})\b", token.Keyword.Declaration),
            (rf"\b(?:{'|'.join(STAGE_KEYWORDS)})\b", token.Keyword.Declaration),
            (rf"\b(?:{MATRIX_TYPES_RE})\b", token.Keyword.Type),
            (rf"\b(?:{VECTOR_TYPES_RE}|{SCALAR_TYPES_RE})\b", token.Keyword.Type),
            (rf"\b(?:{'|'.join(RESOURCE_TYPES)})\b", token.Keyword.Type),
            (rf"\b(?:{BUILTIN_CTOR_RE})\b", token.Name.Builtin),
            (rf"\b(?:{'|'.join(BUILTIN_CORE + BUILTIN_MATH)})\b", token.Name.Builtin),
            (r"\b(?:true|false)\b", token.Keyword.Constant),
            (rf"(?<=\.){IDENT}\b", token.Name.Attribute),
            (rf"\b({IDENT})(\s*)(?=\()", bygroups(token.Name.Function, token.Whitespace)),
            (r"(?:\+\+|--|\+=|-=|\*=|/=|==|!=|<=|>=|&&|\|\||[-+*/=!<>])", token.Operator),
            (r"[.,;:\[\](){}]", token.Punctuation),
            (rf"\b{IDENT}\b", token.Name),
        ],
        "comment": [
            (r"[^/*]+", token.Comment.Multiline),
            (r"/\*", token.Comment.Multiline, "#push"),
            (r"\*/", token.Comment.Multiline, "#pop"),
            (r"[/*]", token.Comment.Multiline),
        ],
        "preprocessor": [
            (r"\n", token.Whitespace, "#pop"),
            (r"\s+", token.Whitespace),
            (r'"', token.String.Double, "string"),
            (FLOAT_LITERAL_RE, token.Number.Float),
            (EXP_FLOAT_LITERAL_RE, token.Number.Float),
            (INTEGER_LITERAL_RE, token.Number.Integer),
            (rf"\b{IDENT}\b", token.Name.Function),
            (r".", token.Comment.Preproc),
        ],
        "resource-map": [
            (r"\s+", token.Whitespace),
            (rf"\b(?:{'|'.join(RESOURCE_ACCESS_KEYWORDS)})\b", token.Keyword),
            (rf"\b{IDENT}\b", token.Name.Variable),
            (r",", token.Punctuation),
            (r"\]", token.Punctuation, "#pop"),
        ],
        "string": [
            (r'\\.', token.String.Escape),
            (r'"', token.String.Double, "#pop"),
            (r'[^"\\]+', token.String.Double),
        ],
    }


if __name__ == "__main__":
    shader_path = Path(__file__).resolve().parent.parent / "omegasl" / "tests" / "shaders.omegasl"
    print(
        pygments.highlight(
            shader_path.read_text(encoding="utf-8"),
            OmegaSLLexer(),
            TerminalTrueColorFormatter(),
        )
    )
