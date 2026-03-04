
#include <omega-common/common.h>

#include "Toks.def"

#ifndef OMEGASL_LEXER_H
#define OMEGASL_LEXER_H


namespace omegasl {

    struct Tok {
        TokType type;
        OmegaCommon::String str;
        unsigned line = 0;
        unsigned colStart = 0;
        unsigned colEnd = 0;
    };

    class Lexer {
        std::istream *in;
        char c_buffer[TOK_MAX_LEN];
        unsigned currentLine = 1;
        unsigned currentCol = 0;
        unsigned tokenStartLine = 1;
        unsigned tokenStartCol = 0;
        char advanceChar();
    public:
        void setInputStream(std::istream *in);
        Tok nextTok();
        void finishTokenizeFromStream();
    };

    struct TokConsumer {
        virtual void consumeTok(Tok &tok) = 0;
    };
}

#endif //OMEGASL_LEXER_H
