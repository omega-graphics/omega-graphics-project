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

        /// Frontend-only mode. When set, `parseContext` runs the parse +
        /// semantic-analysis + constant-folding passes but skips code
        /// generation and shader-toolchain invocation (`generateDecl` /
        /// `generateInterfaceAndCompileShader`). Used by tooling that needs
        /// the AST and diagnostics without transpiling — chiefly the
        /// language server (`omegasl-lsp`), which must not write temp files
        /// or shell out to dxc/metal/glslc on every edit. The default
        /// (false) preserves the full `omegaslc` pipeline byte-for-byte.
        bool frontendOnly = false;

        /// When non-null and `frontendOnly` is set, each global declaration
        /// that passes semantic analysis is appended here in source order.
        /// The caller owns nothing — these alias the parser's AST nodes,
        /// which live as long as the parse does.
        std::vector<ast::Decl *> * collectedDecls = nullptr;
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
        ast::Stmt *parseSwitchStmtFromBuffer(BlockParseContext & ctxt);

        ast::TypeExpr *buildTypeRef(Tok &first_tok,bool isPointer,bool hasTypeArgs = false,OmegaCommon::Vector<ast::TypeExpr *> * args = nullptr);

        /// One parsed entry from a `( key=value, key=value, ... )` clause
        /// on a resource declaration (static-sampler args, texture
        /// swizzle, future per-resource descriptor fields).
        struct ResourceArgEntry {
            OmegaCommon::String name;
            /// Set when the value parsed as a `TOK_ID` (e.g. `linear`).
            OmegaCommon::String valueStr;
            /// Set when the value parsed as a `TOK_NUM_LITERAL`.
            unsigned valueInt = 0;
            bool valueIsNumeric = false;
            Tok nameTok;
            Tok valueTok;
        };

        /// Shared `( key=value, ... )` parser used by both the static
        /// sampler arm and the texture-swizzle arm of `parseGlobalDecl`.
        /// Precondition: `t` is the token *after* the opening `(`.
        /// Postcondition (true): `t` is the token *after* the closing `)`.
        /// Returns false on parse error (diagnostic already emitted).
        bool parseResourceArgList(Tok & t, OmegaCommon::Vector<ResourceArgEntry> & out);
    public:
        explicit Parser(std::shared_ptr<CodeGen> &gen);
        void parseContext(const ParseContext &ctxt);
        ~Parser();
    };
}

#endif