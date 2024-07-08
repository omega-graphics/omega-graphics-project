#include <string>
#include <vector>
#include <istream>

#include "toks.def"
#include "diagnostics.h"

#ifndef OMEGA_WRAPGEN_LEXER_H
#define  OMEGA_WRAPGEN_LEXER_H

namespace OmegaWrapGen {

    struct Tok {
        TokTy type;
        std::string content;
    };


    class Lexer final {
         std::istream * is;
         DiagnosticBuffer & stream;
    public:
        Lexer(DiagnosticBuffer & stream);
        void setInputStream(std::istream * is);
        Tok nextTok();
        void finish();
    };
};

#endif