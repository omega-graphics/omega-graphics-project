
#include <omega-common/utils.h>
#include <optional>

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
        /// One-token lookahead buffer. `nextTok()` returns this (and clears
        /// it) before reading the stream again. Lets a caller working off
        /// the raw lexer (e.g. `parseGlobalDecl`) peek a single token and
        /// un-consume it when the optional construct it was probing for is
        /// absent — used for the optional `fragment(early_depth)` descriptor.
        std::optional<Tok> putback_;
        char advanceChar();
    public:
        void setInputStream(std::istream *in);
        Tok nextTok();
        /// Push `tok` back so the next `nextTok()` call returns it. At most
        /// one token may be buffered at a time.
        void putBack(Tok tok);
        void finishTokenizeFromStream();
    };

    struct TokConsumer {
        virtual void consumeTok(Tok &tok) = 0;
    };
}

#endif //OMEGASL_LEXER_H
