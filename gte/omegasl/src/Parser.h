#include "Lexer.h"
#include "AST.h"
#include "Error.h"

#ifndef OMEGASL_PARSER_H
#define OMEGASL_PARSER_H

namespace omegasl {

    struct CodeGen;

    struct ParseContext {
        std::istream & in;
        SourceFile * sourceFile = nullptr;
        DiagnosticEngine * diagnostics = nullptr;
    };

    class Sem;

    class Parser {
        std::unique_ptr<Sem> sem;
        std::unique_ptr<Lexer> lexer;
        std::shared_ptr<CodeGen> &gen;

        std::vector<Tok> tokenBuffer;

        unsigned tokIdx;

        std::optional<Tok> putbackTok;

        DiagnosticEngine * diagnostics = nullptr;

        Tok & getTok();
        Tok & aheadTok();

        struct BlockParseContext {
            ast::Scope *parentScope;
            bool inShaderContext;
        };

        ast::Block *parseBlock(Tok & first_tok,BlockParseContext & ctxt);

        bool parseObjectExpr(Tok &first_tok,ast::Expr **expr,ast::Scope *parentScope);
        bool parseArgsExpr(Tok &first_tok,ast::Expr **expr,ast::Scope *parentScope);
        bool parseOpExpr(Tok &first_tok,ast::Expr **expr,ast::Scope *parentScope,int minPrec = 0);

        ast::Expr *parseExpr(Tok &first_tok,ast::Scope *parentScope);
        ast::Decl *parseGenericDecl(Tok &first_tok,BlockParseContext & ctxt);
        ast::Stmt *parseStmt(Tok &first_tok,BlockParseContext & ctxt);
        ast::Decl *parseGlobalDecl();

        void collectTokensUntilEndOfStatement(Tok first_tok);
        unsigned findMatchingParen(unsigned startIdx);
        unsigned findMatchingBrace(unsigned startIdx);
        unsigned findExtentOfStatement(unsigned startIdx);
        ast::Block *parseBlockBodyFromBuffer(unsigned startIdx,unsigned endIdx,BlockParseContext & ctxt);
        ast::Stmt *parseStmtFromBuffer(BlockParseContext & ctxt);
        ast::Stmt *parseIfStmtFromBuffer(BlockParseContext & ctxt);
        ast::Stmt *parseForStmtFromBuffer(BlockParseContext & ctxt);
        ast::Stmt *parseWhileStmtFromBuffer(BlockParseContext & ctxt);

        ast::TypeExpr *buildTypeRef(Tok &first_tok,bool isPointer,bool hasTypeArgs = false,OmegaCommon::Vector<ast::TypeExpr *> * args = nullptr);
    public:
        explicit Parser(std::shared_ptr<CodeGen> &gen);
        void parseContext(const ParseContext &ctxt);
        ~Parser();
    };
}

#endif