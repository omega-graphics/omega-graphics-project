#include <memory>
#include "ast.h"
#include "wrapper_gen.h"
#include "lexer.h"
#include "diagnostics.h"

#ifndef OMEGA_WRAPGEN_PARSER_H
#define OMEGA_WRAPGEN_PARSER_H


namespace OmegaWrapGen {


    class Parser final {
        std::unique_ptr<DiagnosticBuffer> errStream;
        std::unique_ptr<Lexer> lexer;
        TreeConsumer * consumer;
        DeclNode *nextDecl();
    public:
        Parser(TreeConsumer * consumer);
         void setInputStream(std::istream * is);
         void beginParse();
         void finish();
    };


};

#endif