
#include <omega-common/common.h>

#include "Toks.def"

#ifndef OMEGASL_LEXER_H
#define OMEGASL_LEXER_H


namespace omegasl {

    struct Tok {
        TokType type;
        OmegaCommon::String str;

        /*  unsigned line;
         *  unsigned colStart,colEnd;
         */
    };

    class Lexer {
        std::istream *in;
        char c_buffer[TOK_MAX_LEN];
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
