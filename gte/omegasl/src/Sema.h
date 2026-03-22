#include "AST.h"
#include "Error.h"

#ifndef OMEGASL_SEMA_H
#define OMEGASL_SEMA_H

namespace std {
    template<>
    struct equal_to<omegasl::ast::ResourceDecl *> {
        auto operator()(omegasl::ast::ResourceDecl *lhs,omegasl::ast::ResourceDecl *rhs){
            return rhs->name == lhs->name;
        }
    };
}

namespace omegasl {

    enum class AttributeContext : int {
        VertexShaderArgument,
        FragmentShaderArgument,
        ComputeShaderArgument,
        HullShaderArgument,
        DomainShaderArgument,
        StructField
    };

    inline bool isValidAttributeInContext(OmegaCommon::StrRef subject,AttributeContext context){
        if(context == AttributeContext::StructField){
            return (subject == ATTRIBUTE_COLOR) || (subject == ATTRIBUTE_POSITION) || (subject == ATTRIBUTE_TEXCOORD);
        }
        else if(context == AttributeContext::VertexShaderArgument || context == AttributeContext::HullShaderArgument || context == AttributeContext::DomainShaderArgument){
            return (subject == ATTRIBUTE_VERTEX_ID);
        }
        else if(context == AttributeContext::ComputeShaderArgument){
            return (subject == ATTRIBUTE_GLOBALTHREAD_ID) || (subject == ATTRIBUTE_THREADGROUP_ID) || (subject == ATTRIBUTE_LOCALTHREAD_ID);
        }
        else if(context == AttributeContext::FragmentShaderArgument){
            return false;
        }
        else {
            return false;
        };
    }

    struct SemContext {
        OmegaCommon::Vector<ast::Type *> typeMap;

        OmegaCommon::Vector<ast::FuncType *> functionMap;
        std::vector<std::unique_ptr<ast::FuncType>> userFuncTypes;

        OmegaCommon::SetVector<ast::ResourceDecl *> resourceSet;

        OmegaCommon::Map<ast::FuncDecl *,OmegaCommon::Vector<OmegaCommon::String>> funcDeclContextTypeMap;

        OmegaCommon::Vector<OmegaCommon::String> shaders;

        OmegaCommon::Map<OmegaCommon::String,ast::TypeExpr *> variableMap;
    };

    /// @brief Impl of Semantics Provider.
    /// @paragraph CodeGen communicates with this class for type data and TypeExpr evalutation.
    class Sem : public ast::SemFrontend {

        OmegaCommon::Vector<ast::Type *> builtinsTypeMap;

        OmegaCommon::Vector<ast::FuncType *> builtinFunctionMap;

        std::shared_ptr<SemContext> currentContext;

        DiagnosticEngine * diagnostics = nullptr;

    public:
        explicit Sem();

        void setDiagnostics(DiagnosticEngine * d);

        void getStructsInFuncDecl(ast::FuncDecl *funcDecl, std::vector<std::string> &out) override;

        void setSemContext(std::shared_ptr<SemContext> & _currentContext);

        void addTypeToCurrentContext(OmegaCommon::StrRef name, ast::Scope *loc,OmegaCommon::MapVec<OmegaCommon::String,ast::TypeExpr *> & fields);

        bool hasTypeNameInFuncDeclContext(OmegaCommon::StrRef name,ast::FuncDecl *funcDecl);

        void addTypeNameToFuncDeclContext(OmegaCommon::StrRef name,ast::FuncDecl *funcDecl);

        ast::Type * resolveTypeWithExpr(ast::TypeExpr *expr) override;

        ast::FuncType *resolveFuncTypeWithName(OmegaCommon::StrRef name);

        ast::TypeExpr *performSemForDecl(ast::Decl * decl,ast::FuncDecl *funcContext);

        ast::TypeExpr *performSemForExpr(ast::Expr * expr,ast::FuncDecl *funcContext);

        ast::TypeExpr * performSemForStmt(ast::Stmt *stmt,ast::FuncDecl *funcContext);

        ast::TypeExpr * evalExprForTypeExpr(ast::Expr *expr) override;

        ast::TypeExpr * performSemForBlock(ast::Block &block,ast::FuncDecl *funcContext);

        bool performSemForGlobalDecl(ast::Decl *decl);
    };
}

#endif
