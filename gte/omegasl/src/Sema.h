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
            return (subject == ATTRIBUTE_COLOR) || (subject == ATTRIBUTE_POSITION)
                || (subject == ATTRIBUTE_TEXCOORD) || (subject == ATTRIBUTE_DEPTH)
                || (subject == ATTRIBUTE_OUTPUT_COVERAGE)
                || (subject == ATTRIBUTE_CLIP_DISTANCE) || (subject == ATTRIBUTE_CULL_DISTANCE);
        }
        else if(context == AttributeContext::VertexShaderArgument || context == AttributeContext::HullShaderArgument || context == AttributeContext::DomainShaderArgument){
            return (subject == ATTRIBUTE_VERTEX_ID);
        }
        else if(context == AttributeContext::ComputeShaderArgument){
            return (subject == ATTRIBUTE_GLOBALTHREAD_ID) || (subject == ATTRIBUTE_THREADGROUP_ID) || (subject == ATTRIBUTE_LOCALTHREAD_ID);
        }
        else if(context == AttributeContext::FragmentShaderArgument){
            return (subject == ATTRIBUTE_FRONTFACING) || (subject == ATTRIBUTE_SAMPLEINDEX)
                || (subject == ATTRIBUTE_INPUT_COVERAGE);
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

        /// §1.7 — names of internal structs that carry a `CullDistance` field.
        /// The resolved `ast::Type` drops field-attribute metadata, so STRUCT_DECL
        /// records the name here and SHADER_DECL flags
        /// `OMEGASL_FEATURE_BIT_CULL_DISTANCE` on a shader whose return struct is
        /// in this set. The FeatureScanner only *unions* `usedFeatures`, so the
        /// bit set here survives the scan and drives the portability warning.
        OmegaCommon::Vector<OmegaCommon::String> cullDistanceStructs;

        /// §3.5 — definition tracking lives on the FuncType itself
        /// (`hasDefinition`). Two overloads with the same name need
        /// independent flags, which the old `Vector<String>` couldn't
        /// give us — it would false-positive a duplicate the moment
        /// the second overload was defined.

        /// §3.6 — value side of `variableMap`. Carries the variable's
        /// resolved type expression alongside any qualifiers that affect
        /// later semantic checks. Today the only qualifier is `isConst`
        /// (set when the local was declared `const T x = …`); it gates
        /// assignment-to-LHS validation in `performSemForExpr`.
        struct VarBinding {
            ast::TypeExpr *type = nullptr;
            bool isConst = false;
        };
        OmegaCommon::Map<OmegaCommon::String,VarBinding> variableMap;
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

        /// §3.5 — collect every FuncType registered under `name`,
        /// across both the builtin map and the current user-function
        /// map. Order is builtins-first, user-overloads-after, in
        /// registration order. Used by the call-expression resolver
        /// to drive overload selection and by the FUNC_DECL handler
        /// to decide whether a new declaration is a redeclaration of
        /// an existing signature or a fresh overload.
        OmegaCommon::Vector<ast::FuncType *> resolveFuncCandidatesByName(OmegaCommon::StrRef name);

        /// §3.5 — exact-match overload resolution. Walks `candidates`
        /// (typically the output of `resolveFuncCandidatesByName`),
        /// matches by parameter arity and per-parameter `TypeExpr::compare`,
        /// and returns the unique match or nullptr. Implicit numeric
        /// conversions are out of scope for this pass — exact match
        /// only — per the design call. Builtin candidates are skipped
        /// during matching (their dispatch is handled elsewhere).
        ast::FuncType *resolveOverload(const OmegaCommon::Vector<ast::FuncType *> &candidates,
                                       const OmegaCommon::Vector<ast::TypeExpr *> &argTypes);

        ast::TypeExpr *performSemForDecl(ast::Decl * decl,ast::FuncDecl *funcContext);

        ast::TypeExpr *performSemForExpr(ast::Expr * expr,ast::FuncDecl *funcContext);

        /// Validate a (sampler, texture, coord) triple for `sample`-shaped
        /// builtins. Returns true on success and writes the resolved sampler
        /// and texture types to the out-params (either may be null to skip).
        /// Emits diagnostics on failure.
        bool validateSampleTriple(ast::CallExpr *_expr,
                                  OmegaCommon::StrRef funcName,
                                  ast::FuncDecl *funcContext,
                                  ast::Type **outSamplerTy,
                                  ast::Type **outTexTy);

        ast::TypeExpr * performSemForStmt(ast::Stmt *stmt,ast::FuncDecl *funcContext);

        ast::TypeExpr * evalExprForTypeExpr(ast::Expr *expr) override;

        ast::TypeExpr * performSemForBlock(ast::Block &block,ast::FuncDecl *funcContext);

        bool performSemForGlobalDecl(ast::Decl *decl);
    };
}

#endif
