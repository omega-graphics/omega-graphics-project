#include "parser.h"
#include <memory>
#include <iostream>

namespace OmegaWrapGen {

    /// Tree Builder

    class TreeBuilder {
        Lexer *lexer;
        DiagnosticBuffer & errStream;
        Type *buildType(Tok & first_tok);
        DeclNode *buildDecl(Tok & first_tok,TreeScope *parentScope);
        Tok nextTok();
    public:
        Tok currentTok;

        TreeBuilder(Lexer *lexer,DiagnosticBuffer & errStream);
        DeclNode *nextDecl(bool *hasErrored);
    };

    struct ParserError : public Diagnostic {
        OmegaCommon::String message;
        void format(std::ostream &out) override {
            out << "\x1b[31mPARSE ERROR\x1b[0m:" << message << std::flush;
        };
        bool isError() override {
            return true;
        };
        ParserError(const char *message):message(message){

        };
    };


    TreeBuilder::TreeBuilder(Lexer *lexer,DiagnosticBuffer & errStream):lexer(lexer),errStream(errStream){

    };

    #define PARSER_ERROR_PUSH(m) errStream.push(new ParserError(m));
    #define ERROR_RETURN return nullptr
    #define EXPECTED_ID() PARSER_ERROR_PUSH("Expected Identifier") ERROR_RETURN
    #define EXPECTED_KW() PARSER_ERROR_PUSH("Expected Keyword") ERROR_RETURN
    #define EXPECTED_LBRACE() PARSER_ERROR_PUSH("Expected LBrace") ERROR_RETURN
    #define EXPECTED_RBRACE() PARSER_ERROR_PUSH("Expected RBrace") ERROR_RETURN
    #define EXPECTED_LPAREN() PARSER_ERROR_PUSH("Expected LParen") ERROR_RETURN
    #define EXPECTED_RPAREN() PARSER_ERROR_PUSH("Expected RParen") ERROR_RETURN
    #define EXPECTED_COLON() PARSER_ERROR_PUSH("Expected Colon") ERROR_RETURN
    #define EXPECTED_STR() PARSER_ERROR_PUSH("Expected String Literal") ERROR_RETURN

    #define EXPECTED_KW_EXACT(name) PARSER_ERROR_PUSH("Expected Keyword :" name) ERROR_RETURN


    Type *TreeBuilder::buildType(Tok & first_tok){
        bool isConst = false,isReference = false,isPointer = false;
//        std::cout << "Type Tok 1:" << first_tok.type << first_tok.content << std::endl;

        if(first_tok.type == TOK_KW){
            if(first_tok.content == KW_CONST){
                isConst = true;
            }
            else {
                EXPECTED_KW_EXACT("const");
            }
            first_tok = nextTok();
        }

        if(first_tok.type != TOK_ID){
            EXPECTED_ID();
        };

        OmegaCommon::String type_name = first_tok.content;

        if(type_name == stdtypes::VOID->getName().data()){
            return stdtypes::VOID;
        }
        else if(type_name == stdtypes::INT->getName().data()){
            return stdtypes::INT;
        }


        first_tok = nextTok();
        if(first_tok.type == TOK_ASTERISK){
            isPointer = true;
        }
        else if(first_tok.type == TOK_AMP){
            isReference = true;
        }
        

        auto t = Type::Create(type_name,isConst,isPointer,isReference);
        return t;
    };

    Tok TreeBuilder::nextTok(){
        auto t = lexer->nextTok();
        while(t.type == TOK_LINECOMMENT){
            t = lexer->nextTok();
        };
        return t;
    };

    DeclNode * TreeBuilder::buildDecl(Tok &first_tok,TreeScope *parentScope){
        if(first_tok.type == TOK_KW){
            DeclNode *node;
            if(first_tok.content == KW_CLASS){
                ClassDeclNode *class_node = new ClassDeclNode();
                class_node->type = CLASS_DECL;
                first_tok = nextTok();
                if(first_tok.type != TOK_ID){
                    EXPECTED_ID();
                };
                class_node->name = first_tok.content;
                first_tok = nextTok();

                if(first_tok.type != TOK_LBRACE){
                    EXPECTED_LBRACE();
                };

                first_tok = nextTok();
                auto class_scope = new TreeScope {TreeScope::Class,class_node->name,parentScope};
                while(first_tok.type != TOK_RBRACE){
                    std::cout << first_tok.type << "Content:" << first_tok.content << std::endl;
                    auto child_node = buildDecl(first_tok,class_scope);
                    if(!child_node){
                        ERROR_RETURN;
                    };
                    if(child_node->type == FUNC_DECL){
                        FuncDeclNode *f = (FuncDeclNode *)child_node;
                        if(f->isStatic)
                            class_node->staticMethods.push_back(f);
                        else 
                            class_node->instMethods.push_back(f);
                    };
                    first_tok = nextTok();
                };
                node = class_node;
            }
            else if(first_tok.content == KW_FUNC){
                FuncDeclNode *func_decl = new FuncDeclNode();
                func_decl->type = FUNC_DECL;

                first_tok = nextTok();
                if(first_tok.type != TOK_ID){
                    EXPECTED_ID();
                };
                func_decl->name = first_tok.content;
                first_tok = nextTok();
                if(first_tok.type != TOK_LPAREN){
                    EXPECTED_LPAREN();
                };
                first_tok = nextTok();
                while(first_tok.type != TOK_RPAREN){

                    if(first_tok.type != TOK_ID){
                        EXPECTED_ID();
                    };
                    OmegaCommon::String name = first_tok.content;
                    first_tok = nextTok();

                    if(first_tok.type != TOK_COLON){
                        EXPECTED_COLON();
                    };

                    first_tok = nextTok();
                    auto _arg_ty = buildType(first_tok);

                    if(!_arg_ty) {
                        ERROR_RETURN;
                    }

                    func_decl->params.insert(std::make_pair(name,_arg_ty));
                    first_tok = nextTok();
                };

                first_tok = nextTok();
                auto _ty = buildType(first_tok);
                if(!_ty){
                    ERROR_RETURN;
                };
                func_decl->returnType = _ty;

                node = func_decl;
            }
            else if(first_tok.content == KW_NAMESPACE){
                auto namespace_decl = new NamespaceDeclNode();
                namespace_decl->type = NAMESPACE_DECL;

                first_tok = nextTok();

                if(first_tok.type != TOK_ID){
                    EXPECTED_ID();
                };
                namespace_decl->name = first_tok.content;
                first_tok = nextTok();

                if(first_tok.type != TOK_LBRACE){
                    EXPECTED_LBRACE();
                };

                first_tok = nextTok();
                auto namespace_scope = new TreeScope {TreeScope::Namespace,namespace_decl->name,parentScope};
                while(first_tok.type != TOK_RBRACE){
                    auto decl = buildDecl(first_tok,namespace_scope);
                    if(!decl){
                        ERROR_RETURN;
                    };
                    namespace_decl->body.push_back(decl);
                    first_tok = nextTok();
                }
                node = namespace_decl;
            }
            else if(first_tok.content == KW_HEADER){
                auto * header_decl = new HeaderDeclNode();
                header_decl->type = HEADER_DECL;
                first_tok = nextTok();
//                std::cout << first_tok.type << std::endl;
                if(first_tok.type != TOK_STRLITERAL){
                    EXPECTED_STR();
                }
                header_decl->name = first_tok.content;
                node = header_decl;
            };

            node->scope = parentScope;
            return node;
        }
        else {
            /// Throw Error
//            std::cout << first_tok.type << std::endl;
            EXPECTED_KW();
        };

    };

    DeclNode * TreeBuilder::nextDecl(bool *hasErrored){
        *hasErrored = false;

        currentTok = nextTok();
        std::cout << currentTok.type << std::endl;
        if(currentTok.type == TOK_EOF){
            return nullptr;
        };
        auto res = buildDecl(currentTok,GLOBAL_SCOPE);
        if(!res)
            *hasErrored = true;
        return res;
    };




    /// Tree Semantics

    struct SemanticsSymTable {
        struct Symbol {
            typedef enum : int {
                Class,
                Interface,
                Func,
                Namespace
            } Ty;
            OmegaCommon::String name;
        };
        OmegaCommon::MapVec<Symbol,TreeScope *> symbol_map;
    }; 

    struct TreeSemanticsContext {
        std::unique_ptr<SemanticsSymTable> symbolTable;
    };


    class TreeSemantics {
        DiagnosticBuffer & errStream;
        TreeSemanticsContext *context;
    public:
        void setContext(TreeSemanticsContext *context);
        bool checkDeclInContext(DeclNode *decl);
        TreeSemantics(DiagnosticBuffer & errStream);
    };

    TreeSemantics::TreeSemantics(DiagnosticBuffer & errStream):
    errStream(errStream),
    context(nullptr){

    };

    void TreeSemantics::setContext(TreeSemanticsContext *context){
        this->context = context;
    };

    bool TreeSemantics::checkDeclInContext(DeclNode *decl){
        switch (decl->type) {
            case CLASS_DECL : {
                auto class_decl = (ClassDeclNode *)decl;
                
                break;
            }
            case FUNC_DECL : {
                break;
            }
        }
        return true;
    };






    /// Core Parser

   static std::unique_ptr<TreeSemantics> semantics;
    static std::unique_ptr<TreeBuilder> builder;



    Parser::Parser(TreeConsumer *consumer):
    consumer(consumer),
    errStream(std::make_unique<DiagnosticBuffer>()),
    lexer(std::make_unique<Lexer>(*errStream)){

        builder = std::make_unique<TreeBuilder>(lexer.get(),*errStream);
        semantics = std::make_unique<TreeSemantics>(*errStream);
    };

    void Parser::setInputStream(std::istream *is){
        this->lexer->setInputStream(is);
    };

    void Parser::beginParse(){
        DeclNode *node;
        bool err = false;
        while(((node = builder->nextDecl(&err)) != nullptr) && (!err)){
            consumer->consumeDecl(node);
        };
        // gen->finish();
    };

    void Parser::finish(){
        this->lexer->finish();
        if(errStream->hasErrored()){
            errStream->logAll();
        };
    };

};