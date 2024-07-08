#include "Lexer.h"
#include "AST.h"

#ifndef OMEGASL_PARSER_H
#define OMEGASL_PARSER_H

namespace omegasl {

    struct CodeGen;

    struct ParseContext {
        std::istream & in;
    };

    class Sem;

    class Parser {
        std::unique_ptr<Sem> sem;
        std::unique_ptr<Lexer> lexer;
        std::shared_ptr<CodeGen> &gen;

        std::vector<Tok> tokenBuffer;

        unsigned tokIdx;

        Tok & getTok();
        Tok & aheadTok();

        struct BlockParseContext {
            ast::Scope *parentScope;
            bool inShaderContext;
        };

        ast::Block *parseBlock(Tok & first_tok,BlockParseContext & ctxt);

        bool parseObjectExpr(Tok &first_tok,ast::Expr **expr,ast::Scope *parentScope);
        bool parseArgsExpr(Tok &first_tok,ast::Expr **expr,ast::Scope *parentScope);
        bool parseOpExpr(Tok &first_tok,ast::Expr **expr,ast::Scope *parentScope);

        ast::Expr *parseExpr(Tok &first_tok,ast::Scope *parentScope);
        ast::Decl *parseGenericDecl(Tok &first_tok,BlockParseContext & ctxt);
        ast::Stmt *parseStmt(Tok &first_tok,BlockParseContext & ctxt);
        ast::Decl *parseGlobalDecl();

        ast::TypeExpr *buildTypeRef(Tok &first_tok,bool isPointer,bool hasTypeArgs = false,OmegaCommon::Vector<ast::TypeExpr *> * args = nullptr);
    public:
        explicit Parser(std::shared_ptr<CodeGen> &gen);
        void parseContext(const ParseContext &ctxt);
        ~Parser();
    };
}

#endif