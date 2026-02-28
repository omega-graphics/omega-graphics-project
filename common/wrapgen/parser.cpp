#include "parser.h"
#include <memory>
#include <iostream>
#include <sstream>
#include <vector>

namespace OmegaWrapGen {

    /// Tree Builder

    class TreeBuilder {
        Lexer *lexer;
        DiagnosticBuffer & errStream;
        bool hasBufferedTok = false;
        Tok bufferedTok;
        Type *buildType(Tok & first_tok);
        DeclNode *buildDecl(Tok & first_tok,TreeScope *parentScope);
        void unreadTok(const Tok &tok);
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
    #define EXPECTED_RBRACKET() PARSER_ERROR_PUSH("Expected RBracket") ERROR_RETURN

    #define EXPECTED_KW_EXACT(name) PARSER_ERROR_PUSH("Expected Keyword :" name) ERROR_RETURN
    #define EXPECTED_TYPE() PARSER_ERROR_PUSH("Expected Type Name") ERROR_RETURN


    static inline Type *matchBuiltinType(const OmegaCommon::String &type_name){
        if(type_name == stdtypes::VOID->getName().data()){
            return stdtypes::VOID;
        }
        if(type_name == stdtypes::INT->getName().data()){
            return stdtypes::INT;
        }
        if(type_name == stdtypes::FLOAT->getName().data()){
            return stdtypes::FLOAT;
        }
        if(type_name == stdtypes::LONG->getName().data()){
            return stdtypes::LONG;
        }
        if(type_name == stdtypes::DOUBLE->getName().data()){
            return stdtypes::DOUBLE;
        }
        if(type_name == stdtypes::CHAR->getName().data()){
            return stdtypes::CHAR;
        }
        if(type_name == stdtypes::STRING->getName().data()){
            return stdtypes::STRING;
        }
        return nullptr;
    }

    Type *TreeBuilder::buildType(Tok & first_tok){
        bool isConst = false;
//        std::cout << "Type Tok 1:" << first_tok.type << first_tok.content << std::endl;

        if(first_tok.type == TOK_KW){
            if(first_tok.content == KW_CONST){
                isConst = true;
                first_tok = nextTok();
            }
            else if(!matchBuiltinType(first_tok.content)){
                EXPECTED_TYPE();
            }
        }

        if(first_tok.type != TOK_ID && first_tok.type != TOK_KW){
            EXPECTED_TYPE();
        }

        OmegaCommon::String type_name = first_tok.content;
        Type *resolved_type = nullptr;

        if(first_tok.type == TOK_KW){
            auto *builtin_type = matchBuiltinType(type_name);
            if(!builtin_type){
                EXPECTED_TYPE();
            }
            resolved_type = Type::Create(builtin_type->getName(),isConst,false,false);
        }
        else {
            resolved_type = Type::Create(type_name,isConst,false,false);
        }

        Tok lookahead = nextTok();
        if(lookahead.type == TOK_ASTERISK){
            resolved_type->isPointer = true;
            lookahead = nextTok();
        }
        else if(lookahead.type == TOK_AMP){
            resolved_type->isReference = true;
            lookahead = nextTok();
        }

        while(lookahead.type == TOK_LBRACKET){
            auto close_bracket = nextTok();
            if(close_bracket.type != TOK_RBRACKET){
                EXPECTED_RBRACKET();
            }
            resolved_type = Type::CreateArray(resolved_type);
            lookahead = nextTok();
        }

        unreadTok(lookahead);
        return resolved_type;
    };

    void TreeBuilder::unreadTok(const Tok &tok){
        bufferedTok = tok;
        hasBufferedTok = true;
    }

    Tok TreeBuilder::nextTok(){
        if(hasBufferedTok){
            hasBufferedTok = false;
            return bufferedTok;
        }
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
                    if(first_tok.type == TOK_KW && first_tok.content == KW_FUNC){
                        auto child_node = buildDecl(first_tok,class_scope);
                        if(!child_node){
                            ERROR_RETURN;
                        }
                        if(child_node->type != FUNC_DECL){
                            PARSER_ERROR_PUSH("Class body only supports func declarations and fields");
                            ERROR_RETURN;
                        }

                        FuncDeclNode *f = (FuncDeclNode *)child_node;
                        for(const auto &field : class_node->fields){
                            if(field.name == f->name){
                                PARSER_ERROR_PUSH("Class field and method names must be unique");
                                ERROR_RETURN;
                            }
                        }

                        if(f->isStatic){
                            class_node->staticMethods.push_back(f);
                        }
                        else {
                            class_node->instMethods.push_back(f);
                        }
                        first_tok = nextTok();
                    }
                    else if(first_tok.type == TOK_ID){
                        OmegaCommon::String field_name = first_tok.content;

                        for(const auto &field : class_node->fields){
                            if(field.name == field_name){
                                PARSER_ERROR_PUSH("Duplicate class field name");
                                ERROR_RETURN;
                            }
                        }
                        for(auto method : class_node->instMethods){
                            if(method->name == field_name){
                                PARSER_ERROR_PUSH("Class field and method names must be unique");
                                ERROR_RETURN;
                            }
                        }
                        for(auto method : class_node->staticMethods){
                            if(method->name == field_name){
                                PARSER_ERROR_PUSH("Class field and method names must be unique");
                                ERROR_RETURN;
                            }
                        }

                        first_tok = nextTok();
                        if(first_tok.type != TOK_COLON){
                            EXPECTED_COLON();
                        }

                        first_tok = nextTok();
                        auto field_type = buildType(first_tok);
                        if(!field_type){
                            ERROR_RETURN;
                        }
                        class_node->fields.push_back({field_name,field_type});

                        first_tok = nextTok();
                        if(first_tok.type == TOK_COMMA){
                            first_tok = nextTok();
                        }
                    }
                    else {
                        PARSER_ERROR_PUSH("Class body only supports func declarations and fields");
                        ERROR_RETURN;
                    }
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
                    if(first_tok.type == TOK_COMMA){
                        first_tok = nextTok();
                    }
                };

                first_tok = nextTok();
                auto _ty = buildType(first_tok);
                if(!_ty){
                    ERROR_RETURN;
                };
                func_decl->returnType = _ty;

                node = func_decl;
            }
            else if(first_tok.content == KW_INTERFACE){
                auto *interface_decl = new InterfaceDeclNode();
                interface_decl->type = INTERFACE_DECL;

                first_tok = nextTok();
                if(first_tok.type != TOK_ID){
                    EXPECTED_ID();
                };
                interface_decl->name = first_tok.content;
                first_tok = nextTok();

                if(first_tok.type != TOK_LBRACE){
                    EXPECTED_LBRACE();
                };

                first_tok = nextTok();
                auto interface_scope = new TreeScope {TreeScope::Interface,interface_decl->name,parentScope};
                while(first_tok.type != TOK_RBRACE){
                    auto child_node = buildDecl(first_tok,interface_scope);
                    if(!child_node){
                        ERROR_RETURN;
                    }
                    if(child_node->type != FUNC_DECL){
                        PARSER_ERROR_PUSH("Interface body only supports func declarations");
                        ERROR_RETURN;
                    }
                    interface_decl->instMethods.push_back((FuncDeclNode *)child_node);
                    first_tok = nextTok();
                };
                node = interface_decl;
            }
            else if(first_tok.content == KW_STRUCT){
                auto *struct_decl = new StructDeclNode();
                struct_decl->type = STRUCT_DECL;

                first_tok = nextTok();
                if(first_tok.type != TOK_ID){
                    EXPECTED_ID();
                };
                struct_decl->name = first_tok.content;
                first_tok = nextTok();
                if(first_tok.type != TOK_LBRACE){
                    EXPECTED_LBRACE();
                }

                first_tok = nextTok();
                while(first_tok.type != TOK_RBRACE){
                    if(first_tok.type != TOK_ID){
                        EXPECTED_ID();
                    }

                    OmegaCommon::String field_name = first_tok.content;
                    for(const auto &field : struct_decl->fields){
                        if(field.name == field_name){
                            PARSER_ERROR_PUSH("Duplicate struct field name");
                            ERROR_RETURN;
                        }
                    }

                    first_tok = nextTok();
                    if(first_tok.type != TOK_COLON){
                        EXPECTED_COLON();
                    }

                    first_tok = nextTok();
                    auto field_type = buildType(first_tok);
                    if(!field_type){
                        ERROR_RETURN;
                    }
                    struct_decl->fields.push_back({field_name,field_type});

                    first_tok = nextTok();
                    if(first_tok.type == TOK_COMMA){
                        first_tok = nextTok();
                    }
                };

                node = struct_decl;
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
        typedef enum : int {
            Class,
            Interface,
            Struct,
            Namespace
        } SymbolTy;

        struct Symbol {
            SymbolTy ty;
            OmegaCommon::String name;
            TreeScope *scope;
        };
        OmegaCommon::Vector<Symbol> symbols;
    }; 

    struct TreeSemanticsContext {
        std::unique_ptr<SemanticsSymTable> symbolTable;
    };

    struct SemanticsError : public Diagnostic {
        OmegaCommon::String message;
        explicit SemanticsError(OmegaCommon::StrRef _message):message(_message.data()){
        };
        bool isError() override{
            return true;
        };
        void format(std::ostream &out) override{
            out << "\x1b[31mSEMANTIC ERROR\x1b[0m:" << message.data();
        };
    };

    class TreeSemantics {
        typedef enum : int {
            FieldType,
            FuncParamType,
            FuncReturnType
        } TypeUseSite;

        DiagnosticBuffer & errStream;
        TreeSemanticsContext *context;

        void pushError(const OmegaCommon::String &message);
        void collectDecl(DeclNode *decl);
        bool checkFieldType(Type *type, TreeScope *scope, OmegaCommon::StrRef owner_kind, OmegaCommon::StrRef owner_name, OmegaCommon::StrRef field_name);
        bool checkFuncType(Type *type, TreeScope *scope, TypeUseSite use_site, OmegaCommon::StrRef func_name, OmegaCommon::StrRef arg_name = "");
        bool checkFunctionDecl(FuncDeclNode *func_decl);
        bool checkClassDecl(ClassDeclNode *class_decl);
        bool checkInterfaceDecl(InterfaceDeclNode *interface_decl);
        bool checkStructDecl(StructDeclNode *struct_decl);
        bool checkType(Type *type, TreeScope *scope, TypeUseSite use_site, OmegaCommon::StrRef context_label, OmegaCommon::StrRef detail_label = "");
        bool isBareVoidType(Type *type);
        bool isBuiltinTypeName(OmegaCommon::StrRef type_name);

        typedef enum : int {
            ResolvedType,
            ResolvedNamespace,
            MissingSymbol
        } TypeResolution;
        TypeResolution resolveTypeName(OmegaCommon::StrRef type_name,TreeScope *scope);

        OmegaCommon::String describeScope(TreeScope *scope);
        OmegaCommon::String formatTypeContext(TypeUseSite use_site,OmegaCommon::StrRef context_label,OmegaCommon::StrRef detail_label);
    public:
        void setContext(TreeSemanticsContext *context);
        bool checkDeclInContext(DeclNode *decl);
        bool checkTranslationUnit(const OmegaCommon::Vector<DeclNode *> &decls);
        TreeSemantics(DiagnosticBuffer & errStream);
    };

    TreeSemantics::TreeSemantics(DiagnosticBuffer & errStream):
    errStream(errStream),
    context(nullptr){

    };

    void TreeSemantics::setContext(TreeSemanticsContext *context){
        this->context = context;
    };

    void TreeSemantics::pushError(const OmegaCommon::String &message){
        errStream.push(new SemanticsError(message));
    }

    void TreeSemantics::collectDecl(DeclNode *decl){
        if(!context || !context->symbolTable){
            return;
        }

        auto &symbols = context->symbolTable->symbols;
        switch (decl->type) {
            case CLASS_DECL: {
                auto *class_decl = (ClassDeclNode *)decl;
                symbols.push_back({SemanticsSymTable::Class,class_decl->name,decl->scope});
                break;
            }
            case INTERFACE_DECL: {
                auto *interface_decl = (InterfaceDeclNode *)decl;
                symbols.push_back({SemanticsSymTable::Interface,interface_decl->name,decl->scope});
                break;
            }
            case STRUCT_DECL: {
                auto *struct_decl = (StructDeclNode *)decl;
                symbols.push_back({SemanticsSymTable::Struct,struct_decl->name,decl->scope});
                break;
            }
            case NAMESPACE_DECL: {
                auto *namespace_decl = (NamespaceDeclNode *)decl;
                symbols.push_back({SemanticsSymTable::Namespace,namespace_decl->name,decl->scope});
                for(auto child : namespace_decl->body){
                    collectDecl(child);
                }
                break;
            }
            default: {
                break;
            }
        }
    }

    bool TreeSemantics::isBareVoidType(Type *type){
        return type->getName() == stdtypes::VOID->getName().data() && !type->isPointer && !type->isReference;
    }

    bool TreeSemantics::isBuiltinTypeName(OmegaCommon::StrRef type_name){
        return matchBuiltinType(type_name.data()) != nullptr;
    }

    TreeSemantics::TypeResolution TreeSemantics::resolveTypeName(OmegaCommon::StrRef type_name,TreeScope *scope){
        if(!context || !context->symbolTable){
            return MissingSymbol;
        }

        bool saw_namespace = false;
        auto cursor = scope;
        while(cursor){
            for(auto &sym : context->symbolTable->symbols){
                if(sym.scope == cursor && sym.name == type_name.data()){
                    if(sym.ty == SemanticsSymTable::Namespace){
                        saw_namespace = true;
                    }
                    else {
                        return ResolvedType;
                    }
                }
            }
            cursor = cursor->parentScope;
        }

        return saw_namespace ? ResolvedNamespace : MissingSymbol;
    }

    OmegaCommon::String TreeSemantics::describeScope(TreeScope *scope){
        std::ostringstream out;
        std::vector<OmegaCommon::String> path;
        auto cursor = scope;
        while(cursor && cursor != GLOBAL_SCOPE){
            path.push_back(cursor->name);
            cursor = cursor->parentScope;
        }

        if(path.empty()){
            out << "<global>";
        }
        else {
            for(auto it = path.rbegin();it != path.rend();it++){
                if(it != path.rbegin()){
                    out << "::";
                }
                out << *it;
            }
        }
        return out.str();
    }

    OmegaCommon::String TreeSemantics::formatTypeContext(TypeUseSite use_site,OmegaCommon::StrRef context_label,OmegaCommon::StrRef detail_label){
        std::ostringstream out;
        switch (use_site) {
            case FieldType: {
                out << "field '" << detail_label.data() << "' in " << context_label.data();
                break;
            }
            case FuncParamType: {
                out << "parameter '" << detail_label.data() << "' of function '" << context_label.data() << "'";
                break;
            }
            case FuncReturnType: {
                out << "return type of function '" << context_label.data() << "'";
                break;
            }
        }
        return out.str();
    }

    bool TreeSemantics::checkType(Type *type, TreeScope *scope, TypeUseSite use_site, OmegaCommon::StrRef context_label, OmegaCommon::StrRef detail_label){
        bool ok = true;

        if(type->isArray){
            auto *element_type = type->getElementType();
            if(!element_type){
                pushError("Array type has no element type");
                return false;
            }
            if(isBareVoidType(element_type)){
                std::ostringstream msg;
                msg << "Invalid type 'void[]' used in " << formatTypeContext(use_site,context_label,detail_label) << " (scope: " << describeScope(scope) << ")";
                pushError(msg.str());
                return false;
            }
            ok = checkType(element_type,scope,use_site,context_label,detail_label) && ok;
            return ok;
        }

        if(isBareVoidType(type)){
            if(use_site != FuncReturnType){
                std::ostringstream msg;
                msg << "Invalid type 'void' used in " << formatTypeContext(use_site,context_label,detail_label) << " (scope: " << describeScope(scope) << ")";
                pushError(msg.str());
                ok = false;
            }
            return ok;
        }

        auto type_name = type->getName();
        if(isBuiltinTypeName(type_name)){
            return ok;
        }

        auto resolution = resolveTypeName(type_name,scope);
        if(resolution == TypeResolution::ResolvedType){
            return ok;
        }

        std::ostringstream msg;
        if(resolution == TypeResolution::ResolvedNamespace){
            msg << "Type name '" << type_name.data() << "' resolves to a namespace in " << formatTypeContext(use_site,context_label,detail_label) << " (scope: " << describeScope(scope) << ")";
        }
        else {
            msg << "Unknown type '" << type_name.data() << "' used in " << formatTypeContext(use_site,context_label,detail_label) << " (scope: " << describeScope(scope) << ")";
        }
        pushError(msg.str());
        return false;
    }

    bool TreeSemantics::checkFieldType(Type *type,TreeScope *scope,OmegaCommon::StrRef owner_kind,OmegaCommon::StrRef owner_name,OmegaCommon::StrRef field_name){
        std::ostringstream owner_label;
        owner_label << owner_kind.data() << " '" << owner_name.data() << "'";
        return checkType(type,scope,FieldType,owner_label.str(),field_name);
    }

    bool TreeSemantics::checkFuncType(Type *type,TreeScope *scope,TypeUseSite use_site,OmegaCommon::StrRef func_name,OmegaCommon::StrRef arg_name){
        return checkType(type,scope,use_site,func_name,arg_name);
    }

    bool TreeSemantics::checkFunctionDecl(FuncDeclNode *func_decl){
        bool ok = true;
        for(auto &param : func_decl->params){
            ok = checkFuncType(param.second,func_decl->scope,FuncParamType,func_decl->name,param.first) && ok;
        }
        ok = checkFuncType(func_decl->returnType,func_decl->scope,FuncReturnType,func_decl->name) && ok;
        return ok;
    }

    bool TreeSemantics::checkClassDecl(ClassDeclNode *class_decl){
        bool ok = true;
        for(auto &field : class_decl->fields){
            ok = checkFieldType(field.type,class_decl->scope,"class",class_decl->name,field.name) && ok;
        }
        for(auto method : class_decl->instMethods){
            ok = checkFunctionDecl(method) && ok;
        }
        for(auto method : class_decl->staticMethods){
            ok = checkFunctionDecl(method) && ok;
        }
        return ok;
    }

    bool TreeSemantics::checkInterfaceDecl(InterfaceDeclNode *interface_decl){
        bool ok = true;
        for(auto method : interface_decl->instMethods){
            ok = checkFunctionDecl(method) && ok;
        }
        return ok;
    }

    bool TreeSemantics::checkStructDecl(StructDeclNode *struct_decl){
        bool ok = true;
        for(auto &field : struct_decl->fields){
            ok = checkFieldType(field.type,struct_decl->scope,"struct",struct_decl->name,field.name) && ok;
        }
        return ok;
    }

    bool TreeSemantics::checkDeclInContext(DeclNode *decl){
        switch (decl->type) {
            case CLASS_DECL : {
                auto *class_decl = (ClassDeclNode *)decl;
                return checkClassDecl(class_decl);
                break;
            }
            case FUNC_DECL : {
                auto *func_decl = (FuncDeclNode *)decl;
                return checkFunctionDecl(func_decl);
            }
            case INTERFACE_DECL : {
                auto *interface_decl = (InterfaceDeclNode *)decl;
                return checkInterfaceDecl(interface_decl);
            }
            case STRUCT_DECL : {
                auto *struct_decl = (StructDeclNode *)decl;
                return checkStructDecl(struct_decl);
            }
            case NAMESPACE_DECL : {
                auto *namespace_decl = (NamespaceDeclNode *)decl;
                bool ok = true;
                for(auto child : namespace_decl->body){
                    ok = checkDeclInContext(child) && ok;
                }
                return ok;
            }
            default : {
                break;
            }
        }
        return true;
    };

    bool TreeSemantics::checkTranslationUnit(const OmegaCommon::Vector<DeclNode *> &decls){
        if(!context){
            pushError("Internal semantic context is not set");
            return false;
        }

        if(!context->symbolTable){
            context->symbolTable = std::make_unique<SemanticsSymTable>();
        }
        context->symbolTable->symbols.clear();

        for(auto decl : decls){
            collectDecl(decl);
        }

        bool ok = true;
        for(auto decl : decls){
            ok = checkDeclInContext(decl) && ok;
        }
        return ok;
    }






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
        OmegaCommon::Vector<DeclNode *> decls;
        DeclNode *node;
        bool err = false;
        while((node = builder->nextDecl(&err)) != nullptr){
            if(err){
                return;
            }
            decls.push_back(node);
        }
        if(err){
            return;
        }

        TreeSemanticsContext semantics_context {};
        semantics_context.symbolTable = std::make_unique<SemanticsSymTable>();
        semantics->setContext(&semantics_context);
        if(!semantics->checkTranslationUnit(decls)){
            return;
        }

        if(errStream->hasErrored()){
            return;
        }

        for(auto decl : decls){
            consumer->consumeDecl(decl);
        }
    };

    void Parser::finish(){
        this->lexer->finish();
        hadErrors = errStream->hasErrored();
        if(hadErrors){
            errStream->logAll();
        };
    };

    bool Parser::hasErrors(){
        return hadErrors || errStream->hasErrored();
    }

};
