#include "Sema.h"
#include "Toks.def"

namespace omegasl {

    Sem::Sem():
    builtinsTypeMap({

        ast::builtins::void_type,
        ast::builtins::bool_type,
        ast::builtins::int_type,
        ast::builtins::int2_type,
        ast::builtins::int3_type,
        ast::builtins::int4_type,
        ast::builtins::uint_type,
        ast::builtins::uint2_type,
        ast::builtins::uint3_type,
        ast::builtins::uint4_type,

        ast::builtins::float_type,
        ast::builtins::float2_type,
        ast::builtins::float3_type,
        ast::builtins::float4_type,
        ast::builtins::float2x2_type,
        ast::builtins::float2x3_type,
        ast::builtins::float2x4_type,
        ast::builtins::float3x2_type,
        ast::builtins::float3x3_type,
        ast::builtins::float3x4_type,
        ast::builtins::float4x2_type,
        ast::builtins::float4x3_type,
        ast::builtins::float4x4_type,

        ast::builtins::buffer_type,
        ast::builtins::texture1d_type,
        ast::builtins::texture2d_type,
        ast::builtins::texture3d_type,
        ast::builtins::sampler1d_type,
        ast::builtins::sampler2d_type,
        ast::builtins::sampler3d_type
        }),
        builtinFunctionMap({

            ast::builtins::make_float2,
            ast::builtins::make_float3,
            ast::builtins::make_float4,
            ast::builtins::make_int2,
            ast::builtins::make_int3,
            ast::builtins::make_int4,
            ast::builtins::make_uint2,
            ast::builtins::make_uint3,
            ast::builtins::make_uint4,
            ast::builtins::make_float2x2,
            ast::builtins::make_float2x3,
            ast::builtins::make_float2x4,
            ast::builtins::make_float3x2,
            ast::builtins::make_float3x3,
            ast::builtins::make_float3x4,
            ast::builtins::make_float4x2,
            ast::builtins::make_float4x3,
            ast::builtins::make_float4x4,
            ast::builtins::dot,
            ast::builtins::cross,
            ast::builtins::sample,
            ast::builtins::write,
            ast::builtins::read
        }),currentContext(nullptr){

    };

    void Sem::setDiagnostics(DiagnosticEngine * d) { diagnostics = d; }

    void Sem::getStructsInFuncDecl(ast::FuncDecl *funcDecl, std::vector<std::string> &out) {
        for(auto & t : currentContext->funcDeclContextTypeMap){
            if(t.first == funcDecl){
                out = t.second;
            }
        }
    }

    void Sem::setSemContext(std::shared_ptr<SemContext> & _currentContext){
        currentContext = _currentContext;
    }

    void Sem::addTypeToCurrentContext(OmegaCommon::StrRef name, ast::Scope *loc,OmegaCommon::MapVec<OmegaCommon::String,ast::TypeExpr *> & fields){
        currentContext->typeMap.push_back(new ast::Type {name,loc,false,{},fields});
    }

    bool Sem::hasTypeNameInFuncDeclContext(OmegaCommon::StrRef name,ast::FuncDecl *funcDecl){
        auto decl_res = currentContext->funcDeclContextTypeMap.find(funcDecl);

        if(decl_res == currentContext->funcDeclContextTypeMap.end()){
            return false;
        }

        auto name_it = decl_res->second.begin();
        for(;name_it != decl_res->second.end();name_it++){
            if(name == *name_it){
                break;
            }
        }
        return name_it != decl_res->second.end();
    }

    void Sem::addTypeNameToFuncDeclContext(OmegaCommon::StrRef name,ast::FuncDecl *funcDecl){
        auto type_map_context = currentContext->funcDeclContextTypeMap.find(funcDecl);
        if(type_map_context == currentContext->funcDeclContextTypeMap.end()){
            currentContext->funcDeclContextTypeMap.insert(std::make_pair(funcDecl,OmegaCommon::Vector<OmegaCommon::String>{name}));
        }
        else {
            type_map_context->second.push_back(name);
        }

    }

    ast::Type * Sem::resolveTypeWithExpr(ast::TypeExpr *expr) {

        auto b_type_it = builtinsTypeMap.begin();
        for(;b_type_it != builtinsTypeMap.end();b_type_it++){
            auto & t = *b_type_it;
            if(t->name == expr->name){
                return t;
            }
        }

        auto type_it = currentContext->typeMap.begin();
        for(;type_it != currentContext->typeMap.end();type_it++){
            auto & t = *type_it;
            if(t->name == expr->name){
                return t;
            }
        }

        return nullptr;
    };

    ast::FuncType *Sem::resolveFuncTypeWithName(OmegaCommon::StrRef name){

        auto builitin_func_it = builtinFunctionMap.begin();
        while(builitin_func_it != builtinFunctionMap.end()){
            auto f = *builitin_func_it;
            if(f->name == name){
                break;
            }
            ++builitin_func_it;
        }

        if(builitin_func_it != builtinFunctionMap.end()){
            return *builitin_func_it;
        }

        auto contextual_func_it = currentContext->functionMap.begin();
        while(contextual_func_it != currentContext->functionMap.end()){
            auto f = *contextual_func_it;
            if(f->name == name){
                break;
            }
            ++contextual_func_it;
        }

        if(contextual_func_it != currentContext->functionMap.end()){
            return *contextual_func_it;
        }

        return nullptr;
    }

    /// Check if a resolved type is a numeric scalar (float, int, uint).
    static bool isNumericScalar(ast::Type *t) {
        return t == ast::builtins::float_type || t == ast::builtins::int_type ||
               t == ast::builtins::uint_type;
    }

    /// Check if a resolved type is a matrix type.
    static bool isMatrixType(ast::Type *t) {
        return t == ast::builtins::float2x2_type || t == ast::builtins::float3x3_type || t == ast::builtins::float4x4_type ||
               t == ast::builtins::float2x3_type || t == ast::builtins::float2x4_type ||
               t == ast::builtins::float3x2_type || t == ast::builtins::float3x4_type ||
               t == ast::builtins::float4x2_type || t == ast::builtins::float4x3_type;
    }

    /// Check if a resolved type is a float vector type.
    static bool isFloatVectorType(ast::Type *t) {
        return t == ast::builtins::float2_type || t == ast::builtins::float3_type || t == ast::builtins::float4_type;
    }

    ast::TypeExpr *Sem::performSemForDecl(ast::Decl * decl,ast::FuncDecl *funcContext){
        auto ret = ast::TypeExpr::Create(KW_TY_VOID);
        switch (decl->type) {
            case VAR_DECL : {
                auto _decl = (ast::VarDecl *)decl;
                auto type_res = resolveTypeWithExpr(_decl->typeExpr);
                if(type_res == nullptr){
                    return nullptr;
                }

                OmegaCommon::StrRef t_name = type_res->name;

                if(!type_res->builtin) {
                    if(!hasTypeNameInFuncDeclContext(t_name,funcContext)){
                        addTypeNameToFuncDeclContext(t_name,funcContext);
                    }
                }

                currentContext->variableMap.insert(std::make_pair(_decl->spec.name,_decl->typeExpr));

                if(_decl->spec.initializer.has_value()){
                    auto initType = performSemForExpr(_decl->spec.initializer.value(), funcContext);
                    if(!initType) return nullptr;
                    /// Check initializer type is compatible with declared type.
                    auto initTy = resolveTypeWithExpr(initType);
                    if(initTy && type_res && initTy != type_res){
                        bool compatible = false;
                        /// Allow numeric scalar implicit conversions.
                        if(isNumericScalar(initTy) && isNumericScalar(type_res)) compatible = true;
                        /// Allow scalar-to-vector is not valid, but same-type is fine.
                        if(!compatible){
                            auto e = std::make_unique<TypeError>(std::string("Cannot initialize `") + _decl->typeExpr->name + "` variable with expression of type `" + initType->name + "`");
                            e->loc = _decl->loc.value_or(ErrorLoc{});
                            diagnostics->addError(std::move(e));
                            return nullptr;
                        }
                    }
                }
                break;
            }
            case RETURN_DECL : {
                auto _decl = (ast::ReturnDecl *)decl;
                if(_decl->expr == nullptr){
                    return ast::TypeExpr::Create(KW_TY_VOID);
                }
                return performSemForExpr(_decl->expr,funcContext);
            }
            case IF_STMT : {
                auto _stmt = (ast::IfStmt *)decl;
                if(_stmt->condition && !performSemForExpr(_stmt->condition,funcContext)) return nullptr;
                if(_stmt->thenBlock) for(auto s : _stmt->thenBlock->body) if(!performSemForStmt(s,funcContext)) return nullptr;
                for(auto & branch : _stmt->elseIfs){
                    if(branch.condition && !performSemForExpr(branch.condition,funcContext)) return nullptr;
                    if(branch.block) for(auto s : branch.block->body) if(!performSemForStmt(s,funcContext)) return nullptr;
                }
                if(_stmt->elseBlock) for(auto s : _stmt->elseBlock->body) if(!performSemForStmt(s,funcContext)) return nullptr;
                break;
            }
            case FOR_STMT : {
                auto _stmt = (ast::ForStmt *)decl;
                if(_stmt->init && !performSemForStmt(_stmt->init,funcContext)) return nullptr;
                if(_stmt->condition && !performSemForExpr(_stmt->condition,funcContext)) return nullptr;
                if(_stmt->increment && !performSemForExpr(_stmt->increment,funcContext)) return nullptr;
                if(_stmt->body) for(auto s : _stmt->body->body) if(!performSemForStmt(s,funcContext)) return nullptr;
                break;
            }
            case WHILE_STMT : {
                auto _stmt = (ast::WhileStmt *)decl;
                if(_stmt->condition && !performSemForExpr(_stmt->condition,funcContext)) return nullptr;
                if(_stmt->body) for(auto s : _stmt->body->body) if(!performSemForStmt(s,funcContext)) return nullptr;
                break;
            }
        }
        return ret;
    }

    ast::TypeExpr *Sem::performSemForExpr(ast::Expr * expr,ast::FuncDecl *funcContext){
        auto ret = ast::TypeExpr::Create(KW_TY_VOID);
        auto setAndReturn = [&](ast::TypeExpr *ty) -> ast::TypeExpr* {
            if(ty && expr) expr->resolvedType = ty;
            return ty;
        };
        if(expr->type == ID_EXPR){
            auto _expr = (ast::IdExpr *)expr;

            auto _id_found = currentContext->variableMap.find(_expr->id);
            if(_id_found == currentContext->variableMap.end()){
                auto err = std::make_unique<UndeclaredIdentifier>(_expr->id);
                err->loc = _expr->loc.value_or(ErrorLoc{});
                diagnostics->addError(std::move(err));
                return nullptr;
            }
            else {
                return _id_found->second;
            }


        }
        else if(expr->type == LITERAL_EXPR){
            auto _expr = (ast::LiteralExpr *)expr;
            if(_expr->isInt()){
                return ast::TypeExpr::Create(ast::builtins::int_type);
            }
            else if(_expr->isUint()){
                return ast::TypeExpr::Create(ast::builtins::uint_type);
            }
            else if(_expr->isFloat()){
                return ast::TypeExpr::Create(ast::builtins::float_type);
            }
            else if(_expr->isBool()){
                return ast::TypeExpr::Create(ast::builtins::bool_type);
            }
        }
        else if(expr->type == ARRAY_EXPR){
            auto _expr = (ast::ArrayExpr *)expr;
        }
        else if(expr->type == MEMBER_EXPR){
            auto _expr = (ast::MemberExpr *)expr;
            auto t = performSemForExpr(_expr->lhs,funcContext);

            if(t == nullptr){
                return nullptr;
            }

            auto type_res = resolveTypeWithExpr(t);
            if(!type_res->builtin){
                auto member_found = type_res->fields.find(_expr->rhs_id);
                if(member_found == type_res->fields.end()){
                    auto e = std::make_unique<TypeError>(std::string("Member `") + _expr->rhs_id + "` does not exist on struct " + type_res->name);
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
                else {
                    return member_found->second;
                }
            }
            else {
#define MATCH_CASE(subject,str) if(subject == str){
#define MATCH_CASE_END() }
                OmegaCommon::StrRef subject = _expr->rhs_id;

                if(type_res == ast::builtins::float2_type){

                    MATCH_CASE(subject,"x")
                        return ast::TypeExpr::Create(ast::builtins::float_type);
                    MATCH_CASE_END()

                    MATCH_CASE(subject,"y")
                        return ast::TypeExpr::Create(ast::builtins::float_type);
                    MATCH_CASE_END()

                    MATCH_CASE(subject,"xy")
                        return ast::TypeExpr::Create(type_res);
                    MATCH_CASE_END()

                    {
                        auto e = std::make_unique<TypeError>(std::string(subject) + " does not exist on type `float2`");
                        e->loc = _expr->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                    }
                    return nullptr;
                }
                else if(type_res == ast::builtins::float3_type){
                    MATCH_CASE(subject,"x")
                    return ast::TypeExpr::Create(ast::builtins::float_type);
                    MATCH_CASE_END()

                    MATCH_CASE(subject,"y")
                    return ast::TypeExpr::Create(ast::builtins::float_type);
                    MATCH_CASE_END()

                    MATCH_CASE(subject,"z")
                    return ast::TypeExpr::Create(ast::builtins::float_type);
                    MATCH_CASE_END()

                    MATCH_CASE(subject,"xy")
                    return ast::TypeExpr::Create(ast::builtins::float2_type);
                    MATCH_CASE_END()

                    MATCH_CASE(subject,"yz")
                    return ast::TypeExpr::Create(ast::builtins::float2_type);
                    MATCH_CASE_END()

                    MATCH_CASE(subject,"xyz")
                    return ast::TypeExpr::Create(type_res);
                    MATCH_CASE_END()

                    {
                        auto e = std::make_unique<TypeError>(std::string(subject) + " does not exist on type `float3`");
                        e->loc = _expr->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                    }
                    return nullptr;
                }
                else if(ast::builtins::float4_type){
                    MATCH_CASE(subject,"x")
                    return ast::TypeExpr::Create(ast::builtins::float_type);
                    MATCH_CASE_END()

                    MATCH_CASE(subject,"y")
                    return ast::TypeExpr::Create(ast::builtins::float_type);
                    MATCH_CASE_END()

                    MATCH_CASE(subject,"z")
                    return ast::TypeExpr::Create(ast::builtins::float_type);
                    MATCH_CASE_END()

                    MATCH_CASE(subject,"w")
                    return ast::TypeExpr::Create(ast::builtins::float_type);
                    MATCH_CASE_END()

                    MATCH_CASE(subject,"xy")
                    return ast::TypeExpr::Create(ast::builtins::float2_type);
                    MATCH_CASE_END()

                    MATCH_CASE(subject,"yz")
                    return ast::TypeExpr::Create(ast::builtins::float2_type);
                    MATCH_CASE_END()

                    MATCH_CASE(subject,"zw")
                    return ast::TypeExpr::Create(ast::builtins::float2_type);
                    MATCH_CASE_END()

                    MATCH_CASE(subject,"xyz")
                    return ast::TypeExpr::Create(ast::builtins::float3_type);
                    MATCH_CASE_END()

                    MATCH_CASE(subject,"yzw")
                    return ast::TypeExpr::Create(ast::builtins::float3_type);
                    MATCH_CASE_END()

                    MATCH_CASE(subject,"xyzw")
                    return ast::TypeExpr::Create(type_res);
                    MATCH_CASE_END()

                    {
                        auto e = std::make_unique<TypeError>(std::string(subject) + " does not exist on type `float4`");
                        e->loc = _expr->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                    }
                    return nullptr;
                }
                else {
                    {
                        auto e = std::make_unique<TypeError>("There are no members available with this type.");
                        e->loc = _expr->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                    }
                    return nullptr;
                }

#undef MATCH_CASE
#undef MATCH_CASE_END
            }
        }
        else if(expr->type == UNARY_EXPR){
            auto _expr = (ast::UnaryOpExpr *)expr;
            return performSemForExpr(_expr->expr,funcContext);
        }
        else if(expr->type == POINTER_EXPR){
            auto _expr = (ast::PointerExpr *)expr;
            return performSemForExpr(_expr->expr,funcContext);
        }
        else if(expr->type == BINARY_EXPR){
            auto _expr = (ast::BinaryExpr *)expr;
            auto rhs_res = performSemForExpr(_expr->rhs,funcContext);
            if(!rhs_res){
                return nullptr;
            }

            auto lhs_res = performSemForExpr(_expr->lhs,funcContext);
            if(!lhs_res){
                return nullptr;
            }

            /// Set resolvedType on sub-expressions for type-aware codegen (HLSL mul()).
            _expr->lhs->resolvedType = lhs_res;
            _expr->rhs->resolvedType = rhs_res;

            if(!rhs_res->compare(lhs_res)){
                auto lhsTy = resolveTypeWithExpr(lhs_res);
                auto rhsTy = resolveTypeWithExpr(rhs_res);
                bool compatible = false;
                if(lhsTy && rhsTy){
                    bool lhsScalar = isNumericScalar(lhsTy);
                    bool rhsScalar = isNumericScalar(rhsTy);
                    /// scalar-vector or vector-scalar
                    if(lhsScalar != rhsScalar) compatible = true;
                    /// matrix * matrix, matrix * vector, vector * matrix, scalar * matrix
                    if(isMatrixType(lhsTy) || isMatrixType(rhsTy)) compatible = true;
                }
                if(!compatible){
                    auto e = std::make_unique<TypeError>("Failed to match type in binary expr.");
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
            }
            /// Return type inference.
            auto lhsTy = resolveTypeWithExpr(lhs_res);
            auto rhsTy = resolveTypeWithExpr(rhs_res);
            /// matrix * vector → vector
            if(lhsTy && rhsTy && isMatrixType(lhsTy) && isFloatVectorType(rhsTy)) return rhs_res;
            /// vector * matrix → vector
            if(lhsTy && rhsTy && isFloatVectorType(lhsTy) && isMatrixType(rhsTy)) return lhs_res;
            /// scalar * non-scalar → non-scalar
            if(lhsTy && isNumericScalar(lhsTy) && rhsTy && !isNumericScalar(rhsTy)) return rhs_res;
            return lhs_res;
        }
        else if(expr->type == INDEX_EXPR){
            auto _expr = (ast::IndexExpr *)expr;
            auto lhs_res = performSemForExpr(_expr->lhs,funcContext);
            if(!lhs_res){
                return nullptr;
            }
            auto idx_expr_res = performSemForExpr(_expr->idx_expr,funcContext);
            if(!idx_expr_res){
                return nullptr;
            }

            auto _t = resolveTypeWithExpr(lhs_res);

            /// Vector component access: float2/3/4[i] -> float, int2/3/4[i] -> int, uint2/3/4[i] -> uint
            if(_t == ast::builtins::float2_type || _t == ast::builtins::float3_type || _t == ast::builtins::float4_type){
                return ast::TypeExpr::Create(ast::builtins::float_type);
            }
            if(_t == ast::builtins::int2_type || _t == ast::builtins::int3_type || _t == ast::builtins::int4_type){
                return ast::TypeExpr::Create(ast::builtins::int_type);
            }
            if(_t == ast::builtins::uint2_type || _t == ast::builtins::uint3_type || _t == ast::builtins::uint4_type){
                return ast::TypeExpr::Create(ast::builtins::uint_type);
            }

            /// Matrix column access: matNxM[i] -> floatM (column vector)
            if(isMatrixType(_t)){
                /// For NxM matrices, indexing returns a column of M elements.
                /// Square matrices return the matching vector size.
                if(_t == ast::builtins::float2x2_type) return ast::TypeExpr::Create(ast::builtins::float2_type);
                if(_t == ast::builtins::float3x3_type) return ast::TypeExpr::Create(ast::builtins::float3_type);
                if(_t == ast::builtins::float4x4_type) return ast::TypeExpr::Create(ast::builtins::float4_type);
                if(_t == ast::builtins::float2x3_type || _t == ast::builtins::float3x3_type) return ast::TypeExpr::Create(ast::builtins::float3_type);
                if(_t == ast::builtins::float2x4_type || _t == ast::builtins::float3x4_type || _t == ast::builtins::float4x4_type) return ast::TypeExpr::Create(ast::builtins::float4_type);
                if(_t == ast::builtins::float3x2_type || _t == ast::builtins::float4x2_type) return ast::TypeExpr::Create(ast::builtins::float2_type);
                if(_t == ast::builtins::float4x3_type) return ast::TypeExpr::Create(ast::builtins::float3_type);
                return ast::TypeExpr::Create(ast::builtins::float4_type); // fallback
            }

            if(_t != ast::builtins::buffer_type){
                auto e = std::make_unique<TypeError>("Indexing is only supported on buffer, vector, and matrix types."); e->loc = _expr->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
                return nullptr;
            }

            _t = resolveTypeWithExpr(idx_expr_res);
            if(_t != ast::builtins::uint_type && _t != ast::builtins::int_type){
                auto e = std::make_unique<TypeError>("Index of buffer must be an int or uint type."); e->loc = _expr->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
            }

            return lhs_res->args[0];

        }
        else if(expr->type == CALL_EXPR){
            auto _expr = (ast::CallExpr *)expr;

            assert(_expr->callee->type == ID_EXPR);

            auto _id_expr = (ast::IdExpr *)_expr->callee;

            auto func_found = resolveFuncTypeWithName(_id_expr->id);

            if(func_found == nullptr){
                /// Check if it's a known math intrinsic.
                OmegaCommon::StrRef fname = _id_expr->id;
                int expectedArgs = -1; // -1 = unknown function
                bool returnsScalar = false; // true for length() which returns scalar from vector

                /// 1-arg intrinsics (same type in, same type out)
                if(fname == BUILTIN_SIN || fname == BUILTIN_COS || fname == BUILTIN_TAN ||
                   fname == BUILTIN_ASIN || fname == BUILTIN_ACOS || fname == BUILTIN_ATAN ||
                   fname == BUILTIN_SQRT || fname == BUILTIN_ABS || fname == BUILTIN_FLOOR ||
                   fname == BUILTIN_CEIL || fname == BUILTIN_ROUND || fname == BUILTIN_FRAC ||
                   fname == BUILTIN_NORMALIZE ||
                   fname == BUILTIN_EXP || fname == BUILTIN_EXP2 ||
                   fname == BUILTIN_LOG || fname == BUILTIN_LOG2){
                    expectedArgs = 1;
                }
                else if(fname == BUILTIN_LENGTH){
                    expectedArgs = 1;
                    returnsScalar = true;
                }
                /// 2-arg intrinsics
                else if(fname == BUILTIN_ATAN2 || fname == BUILTIN_POW ||
                        fname == BUILTIN_MIN || fname == BUILTIN_MAX ||
                        fname == BUILTIN_STEP || fname == BUILTIN_REFLECT){
                    expectedArgs = 2;
                }
                /// 3-arg intrinsics
                else if(fname == BUILTIN_CLAMP || fname == BUILTIN_LERP ||
                        fname == BUILTIN_SMOOTHSTEP){
                    expectedArgs = 3;
                }

                /// Matrix intrinsics with special return types
                bool isTranspose = (fname == "transpose");
                bool isDeterminant = (fname == "determinant");
                if(isTranspose || isDeterminant){
                    expectedArgs = 1;
                }

                if(expectedArgs > 0 && (int)_expr->args.size() != expectedArgs){
                    auto e = std::make_unique<ArgumentCountMismatch>();
                    e->functionName = fname;
                    e->expected = (unsigned)expectedArgs;
                    e->actual = (unsigned)_expr->args.size();
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                /// Validate all arguments.
                ast::TypeExpr *firstArgType = nullptr;
                for(unsigned i = 0; i < _expr->args.size(); i++){
                    auto argType = performSemForExpr(_expr->args[i],funcContext);
                    if(!argType) return nullptr;
                    if(i == 0) firstArgType = argType;
                }

                /// Return type: special cases first.
                if(isDeterminant && firstArgType){
                    return ast::TypeExpr::Create(ast::builtins::float_type);
                }
                if(isTranspose && firstArgType){
                    /// Transpose return type: NxM → MxN
                    auto argTy = resolveTypeWithExpr(firstArgType);
                    if(argTy == ast::builtins::float2x2_type) return ast::TypeExpr::Create(ast::builtins::float2x2_type);
                    if(argTy == ast::builtins::float3x3_type) return ast::TypeExpr::Create(ast::builtins::float3x3_type);
                    if(argTy == ast::builtins::float4x4_type) return ast::TypeExpr::Create(ast::builtins::float4x4_type);
                    if(argTy == ast::builtins::float2x3_type) return ast::TypeExpr::Create(ast::builtins::float3x2_type);
                    if(argTy == ast::builtins::float2x4_type) return ast::TypeExpr::Create(ast::builtins::float4x2_type);
                    if(argTy == ast::builtins::float3x2_type) return ast::TypeExpr::Create(ast::builtins::float2x3_type);
                    if(argTy == ast::builtins::float3x4_type) return ast::TypeExpr::Create(ast::builtins::float4x3_type);
                    if(argTy == ast::builtins::float4x2_type) return ast::TypeExpr::Create(ast::builtins::float2x4_type);
                    if(argTy == ast::builtins::float4x3_type) return ast::TypeExpr::Create(ast::builtins::float3x4_type);
                    return firstArgType; // fallback
                }
                if(returnsScalar && firstArgType){
                    return ast::TypeExpr::Create(ast::builtins::float_type);
                }
                if(firstArgType){
                    return firstArgType;
                }
                return ret;
            }

            auto reportTypeErr = [&](const std::string& msg) {
                auto e = std::make_unique<TypeError>(msg); e->loc = _expr->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
            };

            /// Check if is builtin.

            if(func_found == ast::builtins::make_float2){

                if(_expr->args.size() != 2){
                    auto e = std::make_unique<ArgumentCountMismatch>(); e->functionName = BUILTIN_MAKE_FLOAT2; e->expected = 2; e->actual = (unsigned)_expr->args.size(); e->loc = _expr->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
                    return nullptr;
                }

                auto first_t_e = performSemForExpr(_expr->args[0],funcContext);
                auto second_t_e = performSemForExpr(_expr->args[1],funcContext);

                if(first_t_e == nullptr || second_t_e == nullptr){
                    return nullptr;
                }

                auto _t = resolveTypeWithExpr(first_t_e);
                if(!isNumericScalar(_t)){ reportTypeErr("1st param of function " + std::string(BUILTIN_MAKE_FLOAT2) + " must be a numeric scalar"); return nullptr; }
                _t = resolveTypeWithExpr(second_t_e);
                if(!isNumericScalar(_t)){ reportTypeErr("2nd param of function " + std::string(BUILTIN_MAKE_FLOAT2) + " must be a numeric scalar"); return nullptr; }

            }
            else if(func_found == ast::builtins::make_float3){
                if(!(_expr->args.size() == 2 || _expr->args.size() == 3)){
                    auto e = std::make_unique<ArgumentCountMismatch>(); e->functionName = BUILTIN_MAKE_FLOAT3; e->expected = 2; e->actual = (unsigned)_expr->args.size(); e->loc = _expr->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
                    return nullptr;
                }

                auto first_t_e = performSemForExpr(_expr->args[0],funcContext);
                auto second_t_e = performSemForExpr(_expr->args[1],funcContext);
                ast::TypeExpr *third_t_e = nullptr;
                if(_expr->args.size() >= 3) third_t_e = performSemForExpr(_expr->args[2],funcContext);

                if(first_t_e == nullptr || second_t_e == nullptr || (_expr->args.size() >= 3 && third_t_e == nullptr)){
                    return nullptr;
                }

                if(_expr->args.size() == 3){
                    auto _t = resolveTypeWithExpr(first_t_e);
                    if(!isNumericScalar(_t)){ reportTypeErr("1st param of function " + std::string(BUILTIN_MAKE_FLOAT3) + " must be a numeric scalar"); return nullptr; }
                    _t = resolveTypeWithExpr(second_t_e);
                    if(!isNumericScalar(_t)){ reportTypeErr("2nd param of function " + std::string(BUILTIN_MAKE_FLOAT3) + " must be a numeric scalar"); return nullptr; }
                    _t = resolveTypeWithExpr(third_t_e);
                    if(!isNumericScalar(_t)){ reportTypeErr("3rd param of function " + std::string(BUILTIN_MAKE_FLOAT3) + " must be a numeric scalar"); return nullptr; }
                }
                else {
                    auto _t = resolveTypeWithExpr(first_t_e);
                    if(!isNumericScalar(_t) && _t != ast::builtins::float2_type){ reportTypeErr("1st param of function " + std::string(BUILTIN_MAKE_FLOAT3) + " must be a numeric scalar or float2"); return nullptr; }
                    auto _first_t = _t;
                    _t = resolveTypeWithExpr(second_t_e);
                    if(!isNumericScalar(_t) && _t != ast::builtins::float2_type){ reportTypeErr("2nd param of function " + std::string(BUILTIN_MAKE_FLOAT3) + " must be a numeric scalar or float2"); return nullptr; }
                    if(_first_t == _t){ reportTypeErr("Invalid args for " + std::string(BUILTIN_MAKE_FLOAT3)); return nullptr; }
                }

            }
            else if(func_found == ast::builtins::make_float4){
                if(_expr->args.size() < 2 || _expr->args.size() > 4){
                    auto e = std::make_unique<ArgumentCountMismatch>(); e->functionName = BUILTIN_MAKE_FLOAT4; e->expected = 2; e->actual = (unsigned)_expr->args.size(); e->loc = _expr->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
                    return nullptr;
                }

                auto first_t_e = performSemForExpr(_expr->args[0],funcContext);
                auto second_t_e = performSemForExpr(_expr->args[1],funcContext);
                ast::TypeExpr *third_t_e = nullptr, *fourth_t_e = nullptr;
                if(_expr->args.size() >= 3) third_t_e = performSemForExpr(_expr->args[2],funcContext);
                if(_expr->args.size() == 4) fourth_t_e = performSemForExpr(_expr->args[3],funcContext);

                if(first_t_e == nullptr || second_t_e == nullptr || (_expr->args.size() >= 3 && third_t_e == nullptr) || (_expr->args.size() == 4 && fourth_t_e == nullptr)){
                    return nullptr;
                }

                auto reportTy = [&](const std::string& msg) {
                    auto e = std::make_unique<TypeError>(msg); e->loc = _expr->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
                };

                if(_expr->args.size() == 4){
                    auto _t = resolveTypeWithExpr(first_t_e);
                    if(!isNumericScalar(_t)){ reportTy("1st param of function " + std::string(BUILTIN_MAKE_FLOAT4) + " must be a numeric scalar"); return nullptr; }
                    _t = resolveTypeWithExpr(second_t_e);
                    if(!isNumericScalar(_t)){ reportTy("2nd param of function " + std::string(BUILTIN_MAKE_FLOAT4) + " must be a numeric scalar"); return nullptr; }
                    _t = resolveTypeWithExpr(third_t_e);
                    if(!isNumericScalar(_t)){ reportTy("3rd param of function " + std::string(BUILTIN_MAKE_FLOAT4) + " must be a numeric scalar"); return nullptr; }
                    _t = resolveTypeWithExpr(fourth_t_e);
                    if(!isNumericScalar(_t)){ reportTy("4th param of function " + std::string(BUILTIN_MAKE_FLOAT4) + " must be a numeric scalar"); return nullptr; }
                }
                else if(_expr->args.size() == 3){
                    auto _t = resolveTypeWithExpr(first_t_e);
                    if(!isNumericScalar(_t) && _t != ast::builtins::float2_type){ reportTy("1st param of " + std::string(BUILTIN_MAKE_FLOAT4) + " must be a numeric scalar or float2"); return nullptr; }
                    auto _t1 = _t;
                    _t = resolveTypeWithExpr(second_t_e);
                    if(!isNumericScalar(_t) && _t != ast::builtins::float2_type){ reportTy("2nd param of " + std::string(BUILTIN_MAKE_FLOAT4) + " must be a numeric scalar or float2"); return nullptr; }
                    _t = resolveTypeWithExpr(third_t_e);
                    if(!isNumericScalar(_t) && _t != ast::builtins::float2_type){ reportTy("3rd param of " + std::string(BUILTIN_MAKE_FLOAT4) + " must be a numeric scalar or float2"); return nullptr; }
                }
                else {
                    auto _t = resolveTypeWithExpr(first_t_e);
                    if(!isNumericScalar(_t) && _t != ast::builtins::float2_type && _t != ast::builtins::float3_type){ reportTy("1st param of " + std::string(BUILTIN_MAKE_FLOAT4) + " must be a numeric scalar, float2, or float3"); return nullptr; }
                    _t = resolveTypeWithExpr(second_t_e);
                    if(!isNumericScalar(_t) && _t != ast::builtins::float2_type && _t != ast::builtins::float3_type){ reportTy("2nd param of " + std::string(BUILTIN_MAKE_FLOAT4) + " must be a numeric scalar, float2, or float3"); return nullptr; }
                }
            }
            /// @brief sample(sampler sampler,texture texture,texcoord coord) function
            else if(func_found == ast::builtins::sample){

                if(_expr->args.size() != 3){
                    auto e = std::make_unique<ArgumentCountMismatch>(); e->functionName = BUILTIN_SAMPLE; e->expected = 3; e->actual = (unsigned)_expr->args.size(); e->loc = _expr->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
                    return nullptr;
                }

                auto first_t_e = performSemForExpr(_expr->args[0],funcContext);
                auto second_t_e = performSemForExpr(_expr->args[1],funcContext);
                auto third_t_e = performSemForExpr(_expr->args[2],funcContext);

                if(first_t_e == nullptr || second_t_e == nullptr || third_t_e == nullptr){
                    return nullptr;
                }

                auto _t = resolveTypeWithExpr(first_t_e);
                if(_t == nullptr){
                    return nullptr;
                }

                if(!(_t == ast::builtins::sampler1d_type || _t == ast::builtins::sampler2d_type || _t == ast::builtins::sampler3d_type)){
                    reportTypeErr("1st param of function " + std::string(BUILTIN_SAMPLE) + " must be a sampler.");
                    return nullptr;
                }

                auto _first_t = _t;
                _t = resolveTypeWithExpr(second_t_e);
                if(_t == nullptr){
                    return nullptr;
                }
                ///sampler1d
                if(_first_t == ast::builtins::sampler1d_type){

                    if(_t != ast::builtins::texture1d_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_SAMPLE) + " must be a texture1d");
                        return nullptr;
                    }

                    _t = resolveTypeWithExpr(third_t_e);
                    if(_t == nullptr){
                        return nullptr;
                    }

                    if(_t != ast::builtins::float_type){
                        reportTypeErr("3rd param of function " + std::string(BUILTIN_SAMPLE) + " must be a float");
                        return nullptr;
                    }

                }
                ///sampler2d
                else if(_first_t == ast::builtins::sampler2d_type){

                    if(_t != ast::builtins::texture2d_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_SAMPLE) + " must be a texture2d");
                        return nullptr;
                    }

                    _t = resolveTypeWithExpr(third_t_e);
                    if(_t == nullptr){
                        return nullptr;
                    }

                    if(_t != ast::builtins::float2_type){
                        reportTypeErr("3rd param of function " + std::string(BUILTIN_SAMPLE) + " must be a float2");
                        return nullptr;
                    }

                }
                ///sampler3d
                else if(_first_t == ast::builtins::sampler3d_type) {
                    if(_t != ast::builtins::texture3d_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_SAMPLE) + " must be a texture3d");
                        return nullptr;
                    }

                    _t = resolveTypeWithExpr(third_t_e);
                    if(_t == nullptr){
                        return nullptr;
                    }

                    if(_t != ast::builtins::float3_type){
                        reportTypeErr("3rd param of function " + std::string(BUILTIN_SAMPLE) + " must be a float3");
                        return nullptr;
                    }
                }
            }
                /// @brief write(texture texture,texcoord coord,float4 data) function
            else if(func_found == ast::builtins::write){

                if(_expr->args.size() != 3){
                    auto e = std::make_unique<ArgumentCountMismatch>(); e->functionName = BUILTIN_WRITE; e->expected = 3; e->actual = (unsigned)_expr->args.size(); e->loc = _expr->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
                    return nullptr;
                }

                auto first_t_e = performSemForExpr(_expr->args[0],funcContext);
                auto second_t_e = performSemForExpr(_expr->args[1],funcContext);
                auto third_t_e = performSemForExpr(_expr->args[2],funcContext);

                if(first_t_e == nullptr || second_t_e == nullptr || third_t_e == nullptr){
                    return nullptr;
                }

                auto _t = resolveTypeWithExpr(first_t_e);
                if(_t == nullptr){
                    return nullptr;
                }

                if(_t == ast::builtins::texture1d_type){

                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr){
                        return nullptr;
                    }

                    if(_t != ast::builtins::float_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_WRITE) + " must be a float");
                        return nullptr;
                    }

                }
                else if(_t == ast::builtins::texture2d_type){
                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr){
                        return nullptr;
                    }

                    if(_t != ast::builtins::float2_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_WRITE) + " must be a float2");
                        return nullptr;
                    }
                }
                else if(_t == ast::builtins::texture3d_type){
                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr){
                        return nullptr;
                    }

                    if(_t != ast::builtins::float3_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_WRITE) + " must be a float3");
                        return nullptr;
                    }
                }
                else {
                    reportTypeErr(std::string(BUILTIN_WRITE) + " expects a texture-type for the first argument.");
                    return nullptr;
                }

                _t = resolveTypeWithExpr(third_t_e);
                if(_t == nullptr){
                    return nullptr;
                }

                if(_t != ast::builtins::float4_type){
                    reportTypeErr("3rd param of function " + std::string(BUILTIN_WRITE) + " must be a float4");
                    return nullptr;
                }

            }
                /// @brief read(texture texture,texcoord coord) function
            else if(func_found == ast::builtins::read){

                if(_expr->args.size() != 2){
                    auto e = std::make_unique<ArgumentCountMismatch>(); e->functionName = BUILTIN_READ; e->expected = 2; e->actual = (unsigned)_expr->args.size(); e->loc = _expr->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
                    return nullptr;
                }

                auto first_t_e = performSemForExpr(_expr->args[0],funcContext);
                auto second_t_e = performSemForExpr(_expr->args[1],funcContext);

                if(first_t_e == nullptr || second_t_e == nullptr){
                    return nullptr;
                }

                auto _t = resolveTypeWithExpr(first_t_e);
                if(_t == nullptr){
                    return nullptr;
                }

                if(_t == ast::builtins::texture1d_type){
                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr) return nullptr;
                    if(_t != ast::builtins::int_type && _t != ast::builtins::uint_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_READ) + " must be an int or uint for texture1d");
                        return nullptr;
                    }
                }
                else if(_t == ast::builtins::texture2d_type){
                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr) return nullptr;
                    if(_t != ast::builtins::int2_type && _t != ast::builtins::uint2_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_READ) + " must be an int2 or uint2 for texture2d");
                        return nullptr;
                    }
                }
                else if(_t == ast::builtins::texture3d_type){
                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr) return nullptr;
                    if(_t != ast::builtins::int3_type && _t != ast::builtins::uint3_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_READ) + " must be an int3 or uint3 for texture3d");
                        return nullptr;
                    }
                }
                else {
                    reportTypeErr(std::string(BUILTIN_READ) + " expects a texture-type for the first argument.");
                    return nullptr;
                }

            }
            else {
                /// General function call type-check: validate argument count and types where possible.
                if(func_found == ast::builtins::dot || func_found == ast::builtins::cross){
                    if(_expr->args.size() != 2){
                        auto e = std::make_unique<ArgumentCountMismatch>(); e->functionName = func_found->name; e->expected = 2; e->actual = (unsigned)_expr->args.size(); e->loc = _expr->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
                        return nullptr;
                    }
                    auto a0 = performSemForExpr(_expr->args[0],funcContext);
                    auto a1 = performSemForExpr(_expr->args[1],funcContext);
                    if(!a0 || !a1) return nullptr;
                    auto t0 = resolveTypeWithExpr(a0), t1 = resolveTypeWithExpr(a1);
                    if(func_found == ast::builtins::cross &&
                       (!(t0 == ast::builtins::float3_type) || !(t1 == ast::builtins::float3_type))){
                        auto e = std::make_unique<TypeError>("cross() requires float3 arguments"); e->loc = _expr->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
                        return nullptr;
                    }
                }
                else {
                    /// User-defined function call: validate argument count against parameter count.
                    if(!func_found->builtin){
                        unsigned expectedCount = (unsigned)func_found->fields.size();
                        unsigned actualCount = (unsigned)_expr->args.size();
                        if(actualCount != expectedCount){
                            auto e = std::make_unique<ArgumentCountMismatch>();
                            e->functionName = func_found->name;
                            e->expected = expectedCount;
                            e->actual = actualCount;
                            e->loc = _expr->loc.value_or(ErrorLoc{});
                            diagnostics->addError(std::move(e));
                            return nullptr;
                        }
                    }
                    for(unsigned i = 0; i < _expr->args.size(); i++){
                        if(performSemForExpr(_expr->args[i],funcContext) == nullptr)
                            return nullptr;
                    }
                }
            }

            return func_found->returnType;

        }
        else if(expr->type == CAST_EXPR){
            auto _expr = (ast::CastExpr *)expr;
            auto inner = performSemForExpr(_expr->expr, funcContext);
            if(inner == nullptr) return nullptr;
            auto targetTy = resolveTypeWithExpr(_expr->targetType);
            if(targetTy == nullptr) return nullptr;
            return _expr->targetType;
        }

        return ret;
    }

    ast::TypeExpr * Sem::performSemForStmt(ast::Stmt *stmt,ast::FuncDecl *funcContext){
        ast::TypeExpr *returnType = nullptr;

        /* EXPR nodes have (type & DECL) == EXPR; declaration nodes have (type & DECL) == DECL */
        if((stmt->type & DECL) == EXPR){
            returnType = performSemForExpr((ast::Expr *)stmt,funcContext);
        }
        else if(stmt->type & DECL){
            returnType = performSemForDecl((ast::Decl *)stmt,funcContext);
        }

        return returnType;
    }

    ast::TypeExpr * Sem::evalExprForTypeExpr(ast::Expr *expr) {
        return performSemForExpr(expr,nullptr);
    }

    ast::TypeExpr * Sem::performSemForBlock(ast::Block &block,ast::FuncDecl *funcContext){

        std::vector<ast::TypeExpr *> allTypes;

        /// Assume void return if block is empty.
        if(block.body.empty()){
            allTypes.push_back(ast::TypeExpr::Create(KW_TY_VOID));
        }
        else {
            bool hasReturn = false;
            for(auto s : block.body){
                auto res = performSemForStmt(s,funcContext);
                if(!res){
                    auto e = std::make_unique<TypeError>("Failed to perform sem on block statement");
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                if(s->type == RETURN_DECL){
                    allTypes.push_back(res);
                    hasReturn = true;
                }
            }

            if(!hasReturn){
                 allTypes.push_back(ast::TypeExpr::Create(KW_TY_VOID));
            }
        }
        /// Pick first type to check with all others.
        auto picked_type = allTypes.front();
        if(allTypes.size() > 1) {
            for(auto t_it = allTypes.begin() + 1;t_it != allTypes.end();++t_it){
                if(!picked_type->compare(*t_it)){
                    auto e = std::make_unique<TypeError>("Inconsistent return types in function block");
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
            }
        }
        return picked_type;
    }

    bool Sem::performSemForGlobalDecl(ast::Decl *decl){
        switch (decl->type) {
            case STRUCT_DECL : {
                auto *_decl = (ast::StructDecl *)decl;
                /// 1. Check TypeMap if type is defined with name already
                ast::TypeExpr *test_expr = ast::TypeExpr::Create(_decl->name);
                auto res = resolveTypeWithExpr(test_expr);
                if(res != nullptr){
                    auto e = std::make_unique<DuplicateDeclaration>(_decl->name);
                    e->loc = _decl->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    delete test_expr;
                    return false;
                }
                delete test_expr;

                bool & isInternal = _decl->internal;
                /// 2. Check struct fields (uniqueness and types).
                OmegaCommon::MapVec<OmegaCommon::String,ast::TypeExpr *> field_types;

                for(auto & f : _decl->fields){
                    if(field_types.find(f.name) != field_types.end()){
                        auto e = std::make_unique<DuplicateDeclaration>(f.name); e->loc = _decl->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
                        return false;
                    }

                    auto field_ty = resolveTypeWithExpr(f.typeExpr);
                    if(field_ty == nullptr){
                        auto e = std::make_unique<TypeError>(std::string("Unknown type `") + f.typeExpr->name + "` for field `" + f.name + "` in struct `" + _decl->name + "`");
                        e->loc = _decl->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return false;
                    }

                    if(f.attributeName.has_value()){
                        if(!isInternal){
                            auto e = std::make_unique<InvalidAttribute>("Cannot use attributes on fields in public structs"); e->loc = _decl->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
                            return false;
                        }

                        if(!isValidAttributeInContext(f.attributeName.value(),AttributeContext::StructField)){
                            auto e = std::make_unique<InvalidAttribute>(std::string("Invalid attribute name `") + f.attributeName.value() + "`"); e->loc = _decl->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
                            return false;
                        }
                    }

                    field_types.insert(std::make_pair(f.name,f.typeExpr));

                }

                /// 3. If all of the above checks succeed, add struct type to TypeMap.
                addTypeToCurrentContext(_decl->name,_decl->scope,field_types);
                break;
            }
            case RESOURCE_DECL : {

                /// 1. Check is resource name is taken
                auto *_decl = (ast::ResourceDecl *)decl;
                if(currentContext->resourceSet.find(_decl) == currentContext->resourceSet.end()){
                    currentContext->resourceSet.push(_decl);
                }
                else {
                    auto e = std::make_unique<DuplicateDeclaration>(_decl->name);
                    e->loc = _decl->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return false;
                }

                /// 2. Check resource type.
                auto ty = resolveTypeWithExpr(_decl->typeExpr);

                if(ty != ast::builtins::buffer_type
                && ty != ast::builtins::texture1d_type
                && ty != ast::builtins::texture2d_type
                && ty != ast::builtins::texture3d_type
                && ty != ast::builtins::sampler2d_type
                && ty != ast::builtins::sampler3d_type){
                    auto e = std::make_unique<TypeError>(std::string("Resource `") + _decl->name + "` is not a valid type. (" + _decl->typeExpr->name + ")");
                    e->loc = _decl->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return false;
                }

                /// 3. (Applies to sampler types) Check sampler state if declared static.

                if(_decl->isStatic && ty != ast::builtins::sampler2d_type
                   && ty != ast::builtins::sampler3d_type){
                    auto e = std::make_unique<TypeError>(std::string("Resource `") + _decl->name + "` with type `" + ty->name + "` cannot be declared static unless it is a sampler type.");
                    e->loc = _decl->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return false;
                }

                if(_decl->isStatic){

                }




                break;
            }
            case FUNC_DECL : {
                auto *_decl = (ast::FuncDecl *)decl;
                /// 1. Param name uniqueness
                OmegaCommon::MapVec<OmegaCommon::String, int> paramNames;
                for(auto & p : _decl->params){
                    if(paramNames.find(p.name) != paramNames.end()){
                        auto e = std::make_unique<DuplicateDeclaration>(p.name); e->loc = _decl->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
                        return false;
                    }
                    paramNames.insert(std::make_pair(p.name, 0));
                }
                /// 2. Validate return type and param types
                auto returnTy = resolveTypeWithExpr(_decl->returnType);
                if(returnTy == nullptr){
                    return false;
                }
                for(auto & p : _decl->params){
                    auto p_ty = resolveTypeWithExpr(p.typeExpr);
                    if(p_ty == nullptr){
                        return false;
                    }
                    currentContext->variableMap.insert(std::make_pair(p.name, p.typeExpr));
                }
                /// 2. Validate body
                auto eval_result = performSemForBlock(*_decl->block, _decl);
                if(eval_result == nullptr){
                    return false;
                }
                /// 3. Build FuncType and register for call resolution
                auto *ft = new ast::FuncType();
                ft->name = _decl->name;
                ft->declaredScope = ast::builtins::global_scope;
                ft->builtin = false;
                ft->returnType = _decl->returnType;
                for(auto & p : _decl->params){
                    ft->fields.insert(std::make_pair(p.name, p.typeExpr));
                }
                currentContext->userFuncTypes.push_back(std::unique_ptr<ast::FuncType>(ft));
                currentContext->functionMap.push_back(ft);
                currentContext->variableMap.clear();
                break;
            }
            case SHADER_DECL : {
                auto *_decl = (ast::ShaderDecl *)decl;
                /// 1. Check if shader has been declared.

                auto findShader = [&](){
                    bool f = false;
                    for(OmegaCommon::StrRef s : currentContext->shaders){
                        if(s == _decl->name){
                            f = true;
                            break;
                        }
                    }
                    return f;
                };

                auto found = findShader();
                if(found){
                    auto e = std::make_unique<DuplicateDeclaration>(_decl->name);
                    e->loc = _decl->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return false;
                }

                auto & shaderType = _decl->shaderType;

                /// 2. Check shader params and pipeline layout (param name uniqueness).
                OmegaCommon::MapVec<OmegaCommon::String, int> paramNames;
                for(auto & p : _decl->params){
                    if(paramNames.find(p.name) != paramNames.end()){
                        auto e = std::make_unique<DuplicateDeclaration>(p.name); e->loc = _decl->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
                        return false;
                    }
                    paramNames.insert(std::make_pair(p.name, 0));
                }

                for(auto & r : _decl->resourceMap){
                    bool found = false;
                    for(auto res : currentContext->resourceSet){
                        if(res->name == r.name){
                            found = true;
                            auto _t = resolveTypeWithExpr(res->typeExpr);
                            if((_t == ast::builtins::sampler2d_type || _t == ast::builtins::sampler3d_type) && r.access != ast::ShaderDecl::ResourceMapDesc::In){
                                auto e = std::make_unique<TypeError>(std::string("In Shader Decl `") + _decl->name + "`, resource `" + r.name + "` with type `" + _t->name + "` can only be granted input access to shader.");
                                e->loc = _decl->loc.value_or(ErrorLoc{});
                                diagnostics->addError(std::move(e));
                                return false;
                            }
                            currentContext->variableMap.insert(std::make_pair(r.name,res->typeExpr));
                            /// Register buffer element type for struct emission in codegen.
                            if(_t == ast::builtins::buffer_type && !res->typeExpr->args.empty()){
                                auto elemTy = resolveTypeWithExpr(res->typeExpr->args[0]);
                                if(elemTy && !elemTy->builtin){
                                    if(!hasTypeNameInFuncDeclContext(elemTy->name,_decl)){
                                        addTypeNameToFuncDeclContext(elemTy->name,_decl);
                                    }
                                }
                            }
                            break;
                        }
                    }
                    if(!found){
                        auto e = std::make_unique<UndeclaredIdentifier>(r.name);
                        e->loc = _decl->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return false;
                    }
                }

                unsigned paramIndex = 0;

                for(auto & p : _decl->params){
                    auto p_type = resolveTypeWithExpr(p.typeExpr);
                    if(p_type == nullptr){
                        return false;
                    }
                    /// Add struct type if param is an internal struct.
                    if(!p_type->builtin){
                        if(!hasTypeNameInFuncDeclContext(p_type->name,_decl)){
                            addTypeNameToFuncDeclContext(p_type->name,_decl);
                        }
                    }

                    if(p.attributeName.has_value()){
                        AttributeContext context = shaderType == ast::ShaderDecl::Vertex? AttributeContext::VertexShaderArgument
                            : shaderType == ast::ShaderDecl::Fragment? AttributeContext::FragmentShaderArgument
                            : shaderType == ast::ShaderDecl::Compute? AttributeContext::ComputeShaderArgument
                            : shaderType == ast::ShaderDecl::Hull? AttributeContext::HullShaderArgument
                            : AttributeContext::DomainShaderArgument;
                        if(!isValidAttributeInContext(p.attributeName.value(),context)){
                            auto e = std::make_unique<InvalidAttribute>(std::string("Attribute `") + p.attributeName.value() + "` is not valid in parameter context.");
                            e->loc = _decl->loc.value_or(ErrorLoc{});
                            diagnostics->addError(std::move(e));
                            return false;
                        }
                        if(shaderType == ast::ShaderDecl::Compute){
                            if(p.attributeName.value() == ATTRIBUTE_GLOBALTHREAD_ID && paramIndex != 0){
                                auto e = std::make_unique<InvalidAttribute>(std::string("Attribute `") + ATTRIBUTE_GLOBALTHREAD_ID + "` must be the first parameter in a compute shader");
                                e->loc = _decl->loc.value_or(ErrorLoc{});
                                diagnostics->addError(std::move(e));
                                return false;
                            }
                            else if(p.attributeName.value() == ATTRIBUTE_LOCALTHREAD_ID && paramIndex != 1){
                                auto e = std::make_unique<InvalidAttribute>(std::string("Attribute `") + ATTRIBUTE_LOCALTHREAD_ID + "` must be the second parameter in a compute shader");
                                e->loc = _decl->loc.value_or(ErrorLoc{});
                                diagnostics->addError(std::move(e));
                                return false;
                            }
                            else if(p.attributeName.value() == ATTRIBUTE_THREADGROUP_ID && paramIndex != 2){
                                auto e = std::make_unique<InvalidAttribute>(std::string("Attribute `") + ATTRIBUTE_THREADGROUP_ID + "` must be the last parameter in a compute shader");
                                e->loc = _decl->loc.value_or(ErrorLoc{});
                                diagnostics->addError(std::move(e));
                                return false;
                            }
                        }
                    }
                    currentContext->variableMap.insert(std::make_pair(p.name,p.typeExpr));
                    paramIndex += 1;
                }

                /// 3. Check function block.
                auto eval_result = performSemForBlock(*_decl->block,_decl);

                if(eval_result == nullptr){
                    return false;
                }



                /// 4. Check return types.
                /// (Vertex shaders can return internal struct types while, fragment shaders return float4 and compute shaders do not return any value.)
                if(shaderType == ast::ShaderDecl::Fragment){
                    if(!_decl->returnType->compare(ast::TypeExpr::Create(ast::builtins::float4_type))){
                        auto e = std::make_unique<TypeError>(std::string("Fragment shader `") + _decl->name + "` must return a float4, not " + _decl->returnType->name);
                        e->loc = _decl->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return false;
                    }
                }

                if(shaderType == ast::ShaderDecl::Compute){
                    if(!_decl->returnType->compare(ast::TypeExpr::Create(ast::builtins::void_type))){
                        auto e = std::make_unique<TypeError>(std::string("Compute shader `") + _decl->name + "` must return void, not " + _decl->returnType->name);
                        e->loc = _decl->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return false;
                    }
                }

                if(!_decl->returnType->compare(eval_result)){
                    auto e = std::make_unique<TypeError>(std::string("In Function Return Type: Failed to match type. (") + _decl->returnType->name + " vs. " + eval_result->name + ")");
                    e->loc = _decl->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return false;
                }


                /// 5. Check returntype is struct type and add struct to shader context;
                auto t = resolveTypeWithExpr(eval_result);
                if(!t->builtin){
                    if(!hasTypeNameInFuncDeclContext(t->name,_decl)){
                        addTypeNameToFuncDeclContext(t->name,_decl);
                    }
                }


                /// 6. Add shader to context
                currentContext->shaders.push_back(_decl->name);
                /// 7. Clear Variable map.
                currentContext->variableMap.clear();
                break;
            }
            default : {
                auto e = std::make_unique<UnexpectedToken>(std::string("Cannot declare ast::Stmt of type: 0x") + std::to_string(decl->type));
                diagnostics->addError(std::move(e));
                return false;
            }
        }
        return true;
    }

}
