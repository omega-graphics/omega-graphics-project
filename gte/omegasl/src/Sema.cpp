#include "Sema.h"
#include "Toks.def"

namespace omegasl {

    Sem::Sem():
    builtinsTypeMap({

        ast::builtins::void_type,
        ast::builtins::bool_type,
        /// §5.2 bool vectors.
        ast::builtins::bool2_type,
        ast::builtins::bool3_type,
        ast::builtins::bool4_type,
        ast::builtins::int_type,
        ast::builtins::int2_type,
        ast::builtins::int3_type,
        ast::builtins::int4_type,
        ast::builtins::uint_type,
        ast::builtins::uint2_type,
        ast::builtins::uint3_type,
        ast::builtins::uint4_type,

        /// §4.1 16-bit numerics. The Sema sees them like any other
        /// scalar/vector — feature-bit gating is the FeatureScanner's
        /// job, not Sema's, so the type just needs to resolve.
        ast::builtins::half_type,
        ast::builtins::half2_type,
        ast::builtins::half3_type,
        ast::builtins::half4_type,
        ast::builtins::short_type,
        ast::builtins::short2_type,
        ast::builtins::short3_type,
        ast::builtins::short4_type,
        ast::builtins::ushort_type,
        ast::builtins::ushort2_type,
        ast::builtins::ushort3_type,
        ast::builtins::ushort4_type,

        /// §4.2 64-bit ints.
        ast::builtins::long_type,
        ast::builtins::long2_type,
        ast::builtins::long3_type,
        ast::builtins::long4_type,
        ast::builtins::ulong_type,
        ast::builtins::ulong2_type,
        ast::builtins::ulong3_type,
        ast::builtins::ulong4_type,

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
        /// §12.2 follow-up — integer matrices (array-lowered per backend).
        ast::builtins::int2x2_type,
        ast::builtins::int2x3_type,
        ast::builtins::int2x4_type,
        ast::builtins::int3x2_type,
        ast::builtins::int3x3_type,
        ast::builtins::int3x4_type,
        ast::builtins::int4x2_type,
        ast::builtins::int4x3_type,
        ast::builtins::int4x4_type,
        ast::builtins::uint2x2_type,
        ast::builtins::uint2x3_type,
        ast::builtins::uint2x4_type,
        ast::builtins::uint3x2_type,
        ast::builtins::uint3x3_type,
        ast::builtins::uint3x4_type,
        ast::builtins::uint4x2_type,
        ast::builtins::uint4x3_type,
        ast::builtins::uint4x4_type,

        ast::builtins::buffer_type,
        ast::builtins::uniform_type,
        ast::builtins::texture1d_type,
        ast::builtins::texture2d_type,
        ast::builtins::texture3d_type,
        ast::builtins::texture1d_array_type,
        ast::builtins::texture2d_array_type,
        ast::builtins::texturecube_type,
        ast::builtins::texturecube_array_type,
        ast::builtins::texture2d_ms_type,
        ast::builtins::texture2d_ms_array_type,
        ast::builtins::sampler1d_type,
        ast::builtins::sampler2d_type,
        ast::builtins::sampler3d_type,
        ast::builtins::samplercube_type
        }),
        builtinFunctionMap({

            ast::builtins::make_float2,
            ast::builtins::make_float3,
            ast::builtins::make_float4,
            ast::builtins::make_bool2,
            ast::builtins::make_bool3,
            ast::builtins::make_bool4,
            ast::builtins::make_int2,
            ast::builtins::make_int3,
            ast::builtins::make_int4,
            ast::builtins::make_uint2,
            ast::builtins::make_uint3,
            ast::builtins::make_uint4,
            ast::builtins::make_half2,
            ast::builtins::make_half3,
            ast::builtins::make_half4,
            ast::builtins::make_short2,
            ast::builtins::make_short3,
            ast::builtins::make_short4,
            ast::builtins::make_ushort2,
            ast::builtins::make_ushort3,
            ast::builtins::make_ushort4,
            ast::builtins::make_long2,
            ast::builtins::make_long3,
            ast::builtins::make_long4,
            ast::builtins::make_ulong2,
            ast::builtins::make_ulong3,
            ast::builtins::make_ulong4,
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
            ast::builtins::sampleLOD,
            ast::builtins::sampleBias,
            ast::builtins::sampleGrad,
            ast::builtins::gather,
            ast::builtins::gatherRed,
            ast::builtins::gatherGreen,
            ast::builtins::gatherBlue,
            ast::builtins::gatherAlpha,
            ast::builtins::calculateLOD,
            ast::builtins::getDimensions,
            ast::builtins::write,
            ast::builtins::read
        }),currentContext(nullptr){

    };

    void Sem::setDiagnostics(DiagnosticEngine * d) { diagnostics = d; }

    namespace {
        /// Coord shape for a `(samplerTy, texTy)` pairing — mirrors the table
        /// used by the existing `sample` branch so the new sampling variants
        /// don't drift. Returns nullptr if the pairing is invalid.
        ast::Type *expectedCoordTypeForSamplerTexture(ast::Type *samplerTy, ast::Type *texTy){
            using namespace ast::builtins;
            if(samplerTy == sampler1d_type){
                if(texTy == texture1d_type) return float_type;
                if(texTy == texture1d_array_type) return float2_type;
            } else if(samplerTy == sampler2d_type){
                if(texTy == texture2d_type) return float2_type;
                if(texTy == texture2d_array_type) return float3_type;
            } else if(samplerTy == sampler3d_type){
                if(texTy == texture3d_type) return float3_type;
            } else if(samplerTy == samplercube_type){
                if(texTy == texturecube_type) return float3_type;
                if(texTy == texturecube_array_type) return float4_type;
            }
            return nullptr;
        }

        /// Gradient (ddx/ddy) rank for `sampleGrad`. The hardware needs a
        /// gradient in the texture's spatial domain, so 1D textures take a
        /// scalar, 2D textures take float2, 3D and cube textures take float3
        /// (cube grads operate in the unrotated 3D direction space).
        ast::Type *gradientTypeForSampler(ast::Type *samplerTy){
            using namespace ast::builtins;
            if(samplerTy == sampler1d_type) return float_type;
            if(samplerTy == sampler2d_type) return float2_type;
            if(samplerTy == sampler3d_type) return float3_type;
            if(samplerTy == samplercube_type) return float3_type;
            return nullptr;
        }

        bool isSamplerType(ast::Type *t){
            using namespace ast::builtins;
            return t == sampler1d_type || t == sampler2d_type
                || t == sampler3d_type || t == samplercube_type;
        }

        bool isMSTextureType(ast::Type *t){
            using namespace ast::builtins;
            return t == texture2d_ms_type || t == texture2d_ms_array_type;
        }

        /// `gather*` is only defined on 2D and cube textures (and their array
        /// forms) on every backend; other shapes have no `Gather` / `gather`
        /// / `textureGather` form to lower to.
        bool isGatherableTextureType(ast::Type *t){
            using namespace ast::builtins;
            return t == texture2d_type || t == texture2d_array_type
                || t == texturecube_type || t == texturecube_array_type;
        }

        /// Swizzle component resolution for builtin vector types. Maps a
        /// vector `Type` to its scalar component type and arity (2/3/4);
        /// returns `scalar == nullptr` for non-vector types. Covers every
        /// landed numeric vector family. The previous MEMBER_EXPR code
        /// hardcoded only float2/3/4 and — via an always-true `float4_type`
        /// test — routed every other vector through the float4 path, so e.g.
        /// `uint2.x` resolved to `float`.
        struct VecComponentInfo { ast::Type *scalar; int arity; };
        VecComponentInfo vectorComponentInfo(ast::Type *t){
            using namespace ast::builtins;
            if(t == float2_type) return {float_type, 2};
            if(t == float3_type) return {float_type, 3};
            if(t == float4_type) return {float_type, 4};
            if(t == int2_type) return {int_type, 2};
            if(t == int3_type) return {int_type, 3};
            if(t == int4_type) return {int_type, 4};
            if(t == uint2_type) return {uint_type, 2};
            if(t == uint3_type) return {uint_type, 3};
            if(t == uint4_type) return {uint_type, 4};
            if(t == half2_type) return {half_type, 2};
            if(t == half3_type) return {half_type, 3};
            if(t == half4_type) return {half_type, 4};
            if(t == short2_type) return {short_type, 2};
            if(t == short3_type) return {short_type, 3};
            if(t == short4_type) return {short_type, 4};
            if(t == ushort2_type) return {ushort_type, 2};
            if(t == ushort3_type) return {ushort_type, 3};
            if(t == ushort4_type) return {ushort_type, 4};
            if(t == long2_type) return {long_type, 2};
            if(t == long3_type) return {long_type, 3};
            if(t == long4_type) return {long_type, 4};
            if(t == ulong2_type) return {ulong_type, 2};
            if(t == ulong3_type) return {ulong_type, 3};
            if(t == ulong4_type) return {ulong_type, 4};
            if(t == bool2_type) return {bool_type, 2};
            if(t == bool3_type) return {bool_type, 3};
            if(t == bool4_type) return {bool_type, 4};
            return {nullptr, 0};
        }

        /// Inverse of vectorComponentInfo: the N-component vector type for a
        /// scalar (N == 1 returns the scalar itself). nullptr if unavailable.
        ast::Type *vectorTypeForScalarArity(ast::Type *scalar, int n){
            using namespace ast::builtins;
            if(n == 1) return scalar;
            if(scalar == float_type) return n==2?float2_type : n==3?float3_type : float4_type;
            if(scalar == int_type) return n==2?int2_type : n==3?int3_type : int4_type;
            if(scalar == uint_type) return n==2?uint2_type : n==3?uint3_type : uint4_type;
            if(scalar == half_type) return n==2?half2_type : n==3?half3_type : half4_type;
            if(scalar == short_type) return n==2?short2_type : n==3?short3_type : short4_type;
            if(scalar == ushort_type) return n==2?ushort2_type : n==3?ushort3_type : ushort4_type;
            if(scalar == long_type) return n==2?long2_type : n==3?long3_type : long4_type;
            if(scalar == ulong_type) return n==2?ulong2_type : n==3?ulong3_type : ulong4_type;
            if(scalar == bool_type) return n==2?bool2_type : n==3?bool3_type : bool4_type;
            return nullptr;
        }

        /// Positional swizzle index for a component char (x/y/z/w → 0/1/2/3),
        /// or -1 for any other char. Color-channel `rgba` swizzles are a
        /// separate, not-yet-supported feature; positional `xyzw` is what the
        /// prior float-only code accepted, and what is preserved here.
        int swizzleComponentIndex(char c){
            switch(c){
                case 'x': return 0;
                case 'y': return 1;
                case 'z': return 2;
                case 'w': return 3;
                default: return -1;
            }
        }
    }

    /// Validate the (sampler, texture, coord) triple shared by `sample`,
    /// `sampleLOD`, `sampleBias`, `sampleGrad`, and `gather*`. On success,
    /// stores the resolved sampler type in `*outSamplerTy` and the texture
    /// type in `*outTexTy` and returns true. On failure, emits a diagnostic
    /// and returns false. Caller is responsible for arg-count checking.
    bool Sem::validateSampleTriple(ast::CallExpr *_expr,
                                   OmegaCommon::StrRef funcName,
                                   ast::FuncDecl *funcContext,
                                   ast::Type **outSamplerTy,
                                   ast::Type **outTexTy){
        auto first_t_e = performSemForExpr(_expr->args[0], funcContext);
        auto second_t_e = performSemForExpr(_expr->args[1], funcContext);
        auto third_t_e = performSemForExpr(_expr->args[2], funcContext);
        if(!first_t_e || !second_t_e || !third_t_e) return false;

        auto reportTypeErr = [&](const std::string &msg){
            auto e = std::make_unique<TypeError>(msg);
            e->loc = _expr->loc.value_or(ErrorLoc{});
            diagnostics->addError(std::move(e));
        };

        auto samplerTy = resolveTypeWithExpr(first_t_e);
        auto texTy = resolveTypeWithExpr(second_t_e);
        auto coordTy = resolveTypeWithExpr(third_t_e);
        if(!samplerTy || !texTy || !coordTy) return false;

        if(!isSamplerType(samplerTy)){
            reportTypeErr("1st param of function " + std::string(funcName) + " must be a sampler.");
            return false;
        }

        if(isMSTextureType(texTy)){
            reportTypeErr(std::string("Multisample textures cannot be sampled with `") + std::string(funcName) + "`; use `read(tex, coord, sample_index)` instead.");
            return false;
        }

        auto expectedCoord = expectedCoordTypeForSamplerTexture(samplerTy, texTy);
        if(!expectedCoord){
            reportTypeErr("2nd param of function " + std::string(funcName) + " is not a texture compatible with the given sampler.");
            return false;
        }

        if(coordTy != expectedCoord){
            reportTypeErr("3rd param of function " + std::string(funcName) + " (coord) does not match the expected shape for the given sampler/texture pair.");
            return false;
        }

        if(outSamplerTy) *outSamplerTy = samplerTy;
        if(outTexTy) *outTexTy = texTy;
        return true;
    }

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

    /// §3.5 — collect every FuncType registered under `name`. The
    /// return order is intentional: builtins first (so dispatch in
    /// the call-expression resolver can short-circuit on the
    /// builtin path before doing user-overload work), then user
    /// overloads in registration order.
    OmegaCommon::Vector<ast::FuncType *>
    Sem::resolveFuncCandidatesByName(OmegaCommon::StrRef name){
        OmegaCommon::Vector<ast::FuncType *> out;
        for(auto *f : builtinFunctionMap){
            if(f->name == name) out.push_back(f);
        }
        for(auto *f : currentContext->functionMap){
            if(f->name == name) out.push_back(f);
        }
        return out;
    }

    /// §3.5 — exact-signature match. Walks user-function candidates
    /// (skipping builtins, which OmegaSL doesn't allow user code to
    /// shadow), comparing parameter arity and per-parameter type via
    /// `TypeExpr::compare`. Implicit numeric coercion is intentionally
    /// not attempted in this pass — the design call was "exact match
    /// for now; revisit when we add implicit conversion." The first
    /// (and, by exact-match construction, only) candidate whose
    /// signature matches is returned. nullptr means "no overload
    /// matches" — the caller decides whether that's an error.
    ast::FuncType *Sem::resolveOverload(
        const OmegaCommon::Vector<ast::FuncType *> &candidates,
        const OmegaCommon::Vector<ast::TypeExpr *> &argTypes){
        for(auto *cand : candidates){
            if(cand->builtin) continue;
            if(cand->paramTypes.size() != argTypes.size()) continue;
            bool match = true;
            for(size_t i = 0; i < argTypes.size(); ++i){
                /// `paramTypes[i]` is the positional TypeExpr the
                /// FuncType stored at registration; `argTypes[i]` is
                /// what the argument's Sema returned. compare() is
                /// structural (name + pointerness). Positional order
                /// is critical, so we deliberately use the ordered
                /// `paramTypes` vector and not the unordered `fields`
                /// map (MapVec is a hash table).
                if(!cand->paramTypes[i]->compare(argTypes[i])){
                    match = false;
                    break;
                }
            }
            if(match) return cand;
        }
        return nullptr;
    }

    /// Check if a resolved type is a numeric scalar.
    /// Includes the new 16-bit (`half`/`short`/`ushort`) and 64-bit
    /// (`long`/`ulong`) families from §4.1/§4.2 so literal coercion
    /// applies uniformly. The runtime feature-bit gate is upstream of
    /// this check (FeatureScanner traces type *use* through declarations
    /// and decides whether to emit the FLOAT16/INT64 portability
    /// warning).
    static bool isNumericScalar(ast::Type *t) {
        using namespace ast::builtins;
        return t == float_type  || t == int_type    || t == uint_type
            || t == half_type   || t == short_type  || t == ushort_type
            || t == long_type   || t == ulong_type;
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

    /// §1.6 — integer scalar or vector (int/uint/short/ushort/long/ulong
    /// family). Integer interstage varyings cannot be interpolated, so
    /// HLSL/GLSL require `flat` on them; Sema enforces that for internal
    /// struct fields. `half` is float-like and interpolates normally, so it
    /// is intentionally excluded.
    static bool isIntegerScalarOrVector(ast::Type *t) {
        using namespace ast::builtins;
        return t == int_type    || t == int2_type    || t == int3_type    || t == int4_type
            || t == uint_type   || t == uint2_type   || t == uint3_type   || t == uint4_type
            || t == short_type  || t == short2_type  || t == short3_type  || t == short4_type
            || t == ushort_type || t == ushort2_type || t == ushort3_type || t == ushort4_type
            || t == long_type   || t == long2_type   || t == long3_type   || t == long4_type
            || t == ulong_type  || t == ulong2_type  || t == ulong3_type  || t == ulong4_type;
    }

    /// §12.2 follow-up — integer matrix types. These are deliberately kept
    /// *out* of `isMatrixType` (which gates the float-only matrix algebra and
    /// the HLSL §12.1 column-major index swap): integer matrices lower to an
    /// array of column-vectors and use natural array indexing on every
    /// backend, so they must never hit the float-matrix rewrite paths.
    static bool isIntegerMatrixType(ast::Type *t) {
        using namespace ast::builtins;
        return t == int2x2_type || t == int2x3_type || t == int2x4_type ||
               t == int3x2_type || t == int3x3_type || t == int3x4_type ||
               t == int4x2_type || t == int4x3_type || t == int4x4_type ||
               t == uint2x2_type || t == uint2x3_type || t == uint2x4_type ||
               t == uint3x2_type || t == uint3x3_type || t == uint3x4_type ||
               t == uint4x2_type || t == uint4x3_type || t == uint4x4_type;
    }

    /// Column vector returned by `m[col]` on an integer matrix: an `intR` /
    /// `uintR` vector where R is the matrix's row count. Returns nullptr for
    /// non-integer-matrix types.
    static ast::Type *integerMatrixColumnVector(ast::Type *t) {
        using namespace ast::builtins;
        if(t == int2x2_type || t == int3x2_type || t == int4x2_type) return int2_type;
        if(t == int2x3_type || t == int3x3_type || t == int4x3_type) return int3_type;
        if(t == int2x4_type || t == int3x4_type || t == int4x4_type) return int4_type;
        if(t == uint2x2_type || t == uint3x2_type || t == uint4x2_type) return uint2_type;
        if(t == uint2x3_type || t == uint3x3_type || t == uint4x3_type) return uint3_type;
        if(t == uint2x4_type || t == uint3x4_type || t == uint4x4_type) return uint4_type;
        return nullptr;
    }

    /// Unwrap a prefix `-` on a numeric literal so `-5` is treated as a
    /// literal for coercion purposes. ConstFold runs after Sema, so at
    /// this point `-5` is still a UnaryOpExpr(LITERAL_EXPR).
    static ast::LiteralExpr *asNumericLiteral(ast::Expr *e) {
        auto isAnyNumeric = [](ast::LiteralExpr *lit) {
            return lit->isInt() || lit->isUint() || lit->isFloat()
                || lit->isLong() || lit->isUlong();
        };
        if(e == nullptr) return nullptr;
        if(e->type == UNARY_EXPR){
            auto *u = static_cast<ast::UnaryOpExpr *>(e);
            if(u->isPrefix && u->op == OP_MINUS && u->expr && u->expr->type == LITERAL_EXPR){
                auto *lit = static_cast<ast::LiteralExpr *>(u->expr);
                if(isAnyNumeric(lit)) return lit;
            }
            return nullptr;
        }
        if(e->type == LITERAL_EXPR){
            auto *lit = static_cast<ast::LiteralExpr *>(e);
            if(isAnyNumeric(lit)) return lit;
        }
        return nullptr;
    }

    /// Literal coercion rules:
    ///   - integer literals (int / uint / long / ulong) coerce to any
    ///     numeric scalar slot. The shader author writes a small constant
    ///     and gets the declared type; out-of-range values are caught by
    ///     the backend compiler, which is honest about hardware limits.
    ///   - float literals coerce to float and half. The `h` suffix is a
    ///     spelling convenience — `1.0` and `1.0h` both initialize a
    ///     `half` slot. They never coerce to ints/uints; that requires
    ///     an explicit cast.
    static bool canCoerceLiteralTo(ast::LiteralExpr *lit, ast::Type *target) {
        using namespace ast::builtins;
        if(lit == nullptr || target == nullptr) return false;
        if(!isNumericScalar(target)) return false;
        if(lit->isFloat()) return target == float_type || target == half_type;
        if(lit->isInt() || lit->isUint() || lit->isLong() || lit->isUlong()) return true;
        return false;
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

                /// §6.1 — `threadgroup` storage is only meaningful inside a
                /// compute shader (every backend's shared-memory class is
                /// compute-only). Reject it anywhere else with a clear
                /// diagnostic rather than emitting source the downstream
                /// compiler would reject opaquely. The parser already keeps
                /// it at the function-body top level (like `const`).
                if(_decl->isThreadgroup){
                    bool inCompute = funcContext && funcContext->type == SHADER_DECL
                        && ((ast::ShaderDecl *)funcContext)->shaderType == ast::ShaderDecl::Compute;
                    if(!inCompute){
                        auto e = std::make_unique<InvalidAttribute>(
                            "`threadgroup` variables are only valid inside a compute shader");
                        e->loc = _decl->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return nullptr;
                    }
                }

                OmegaCommon::StrRef t_name = type_res->name;

                if(!type_res->builtin) {
                    if(!hasTypeNameInFuncDeclContext(t_name,funcContext)){
                        addTypeNameToFuncDeclContext(t_name,funcContext);
                    }
                }

                currentContext->variableMap.insert(std::make_pair(_decl->spec.name,
                    SemContext::VarBinding{ _decl->typeExpr, _decl->isConst }));

                if(_decl->spec.initializer.has_value()){
                    auto initExpr = _decl->spec.initializer.value();
                    auto initType = performSemForExpr(initExpr, funcContext);
                    if(!initType) return nullptr;
                    /// Aggregate (brace) initializers produce void type — allow them
                    /// for struct types without further type checking.
                    bool isAggregateInit = (initExpr->type == ARRAY_EXPR);
                    if(!isAggregateInit){
                        /// Check initializer type is compatible with declared type.
                        auto initTy = resolveTypeWithExpr(initType);
                        if(initTy && type_res && initTy != type_res){
                            bool compatible = false;
                            /// Literal coercion: a numeric scalar literal
                            /// implicitly takes the declared scalar type.
                            /// Non-literal int↔uint↔float mixing is a real
                            /// bug most of the time — require an explicit
                            /// cast for those.
                            if(isNumericScalar(type_res)){
                                auto *lit = asNumericLiteral(initExpr);
                                if(canCoerceLiteralTo(lit, type_res)){
                                    initExpr->resolvedType = _decl->typeExpr;
                                    compatible = true;
                                }
                            }
                            if(!compatible){
                                auto e = std::make_unique<TypeError>(std::string("Cannot initialize `") + _decl->typeExpr->name + "` variable with expression of type `" + initType->name + "`");
                                e->loc = _decl->loc.value_or(ErrorLoc{});
                                diagnostics->addError(std::move(e));
                                return nullptr;
                            }
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
            case BREAK_STMT :
            case CONTINUE_STMT : {
                /// `break` / `continue` carry no subexpressions and produce no type.
                /// Loop-context enforcement is left to the target backend —
                /// HLSL/MSL/GLSL all reject these outside loops at compile time.
                /// `break` is also valid inside a `switch` body; that's
                /// equally a backend concern.
                break;
            }
            case SWITCH_STMT : {
                auto _stmt = (ast::SwitchStmt *)decl;
                if(_stmt->condition){
                    auto condTyExpr = performSemForExpr(_stmt->condition, funcContext);
                    if(!condTyExpr) return nullptr;
                    auto condTy = resolveTypeWithExpr(condTyExpr);
                    /// Restrict the switch condition to integer scalars.
                    /// HLSL/MSL/GLSL accept more, but generated dispatch
                    /// tables overwhelmingly use int/uint, and a tighter
                    /// rule catches obvious bugs (`switch(uvCoord)` etc.).
                    if(condTy != ast::builtins::int_type && condTy != ast::builtins::uint_type){
                        auto e = std::make_unique<TypeError>("`switch` condition must be an int or uint scalar");
                        e->loc = _stmt->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return nullptr;
                    }
                }
                for(auto &sc : _stmt->cases){
                    if(sc.value){
                        /// Case value: integer literal only for v1. The
                        /// const-expr evaluator can be wired in as a
                        /// follow-up — see ConstFold module.
                        auto valTyExpr = performSemForExpr(sc.value, funcContext);
                        if(!valTyExpr) return nullptr;
                        if(sc.value->type != LITERAL_EXPR){
                            auto e = std::make_unique<TypeError>("`case` value must be an integer literal");
                            e->loc = sc.value->loc.value_or(_stmt->loc.value_or(ErrorLoc{}));
                            diagnostics->addError(std::move(e));
                            return nullptr;
                        }
                        auto *lit = (ast::LiteralExpr *)sc.value;
                        if(!lit->isInt() && !lit->isUint()){
                            auto e = std::make_unique<TypeError>("`case` value must be an integer literal");
                            e->loc = sc.value->loc.value_or(_stmt->loc.value_or(ErrorLoc{}));
                            diagnostics->addError(std::move(e));
                            return nullptr;
                        }
                    }
                    for(auto *s : sc.body){
                        if(!performSemForStmt(s, funcContext)) return nullptr;
                    }
                }
                break;
            }
            case DISCARD_STMT : {
                /// `discard;` is only valid inside a fragment shader.
                bool inFragment = false;
                if(funcContext && funcContext->type == SHADER_DECL){
                    inFragment = ((ast::ShaderDecl *)funcContext)->shaderType == ast::ShaderDecl::Fragment;
                }
                if(!inFragment){
                    auto e = std::make_unique<InvalidAttribute>("`discard` is only valid inside a fragment shader");
                    e->loc = decl->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
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
                /// Stamp the type onto the expression node so later
                /// passes — notably the FeatureScanner trigger table —
                /// can read `arg->resolvedType` directly. Other branches
                /// already do this via `setAndReturn`; the ID_EXPR path
                /// historically didn't, which left every identifier-
                /// shaped argument with a null `resolvedType` and made
                /// scanner triggers like `TEXTURECUBE_RW` and the new
                /// `TEXTURE1D_MIP_SAMPLE` silently miss.
                return setAndReturn(_id_found->second.type);
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
            else if(_expr->isLong()){
                return ast::TypeExpr::Create(ast::builtins::long_type);
            }
            else if(_expr->isUlong()){
                return ast::TypeExpr::Create(ast::builtins::ulong_type);
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
            /// Validate each element expression, but the aggregate itself
            /// has no single resolved type — return void to signal aggregate init.
            for(auto & elem : _expr->elm){
                performSemForExpr(elem, funcContext);
            }
            return ast::TypeExpr::Create(KW_TY_VOID);
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
                /// Builtin vector swizzle. Generalized across every numeric
                /// vector family (float / int / uint / half / short / ushort
                /// / long / ulong): the component scalar type and arity come
                /// from `vectorComponentInfo`, each swizzle char is validated
                /// against the arity, and the result is the scalar (1 char) or
                /// the matching N-component vector. Replaces the float-only
                /// hardcoded blocks — and fixes the always-true `float4_type`
                /// test that made `uint2.x` (etc.) resolve to `float`.
                std::string subject(_expr->rhs_id.data(), _expr->rhs_id.size());
                auto info = vectorComponentInfo(type_res);
                if(!info.scalar){
                    auto e = std::make_unique<TypeError>("There are no members available with this type.");
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
                if(subject.empty() || subject.size() > 4){
                    auto e = std::make_unique<TypeError>(
                        "`" + subject + "` is not a valid swizzle (1-4 components of x/y/z/w) on type `" + type_res->name + "`");
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
                for(char c : subject){
                    int comp = swizzleComponentIndex(c);
                    if(comp < 0 || comp >= info.arity){
                        auto e = std::make_unique<TypeError>(
                            std::string("`") + c + "` is not a valid component of type `" + type_res->name + "`");
                        e->loc = _expr->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return nullptr;
                    }
                }
                auto *resultTy = vectorTypeForScalarArity(info.scalar, (int)subject.size());
                if(!resultTy){
                    auto e = std::make_unique<TypeError>(
                        "`" + subject + "` swizzle width is unavailable for type `" + type_res->name + "`");
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
                return ast::TypeExpr::Create(resultTy);
            }
        }
        else if(expr->type == UNARY_EXPR){
            auto _expr = (ast::UnaryOpExpr *)expr;
            /// §3.6 — `++` / `--` mutate their operand; if the operand
            /// is a const local, reject the same way binary assignment
            /// does below. Other unary ops (`!`, `-`, `~`) are
            /// non-mutating and need no check.
            if((_expr->op == OP_PLUSPLUS || _expr->op == OP_MINUSMINUS)
               && _expr->expr->type == ID_EXPR){
                auto *idExpr = (ast::IdExpr *)_expr->expr;
                auto found = currentContext->variableMap.find(idExpr->id);
                if(found != currentContext->variableMap.end() && found->second.isConst){
                    auto e = std::make_unique<TypeError>(
                        std::string("Cannot modify `const` local `") + idExpr->id + "`.");
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
            }
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

            /// §12.2 follow-up — no operation is defined on a *whole* integer
            /// matrix. Algebra (`*`, `+`, …) has no backend support; equality
            /// and whole-matrix assignment would require array ops that the
            /// C-array lowering can't express portably (MSL/HLSL don't assign
            /// raw arrays). Indexing first (`m[col]`, `m[col][row]`) yields a
            /// vector / scalar that flows through normally, so only operands
            /// that are still matrix-typed reach here.
            {
                auto lTy = resolveTypeWithExpr(lhs_res);
                auto rTy = resolveTypeWithExpr(rhs_res);
                if((lTy && isIntegerMatrixType(lTy)) || (rTy && isIntegerMatrixType(rTy))){
                    auto e = std::make_unique<TypeError>(
                        "Integer matrices support indexing and storage only; no "
                        "operator is defined on a whole integer matrix. Index a "
                        "column or element first (e.g. `m[col]`, `m[col][row]`).");
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
            }

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
                /// Literal coercion: a numeric scalar literal on one side
                /// takes the other side's scalar type. Non-literal int↔uint
                /// (and int↔float) mixing stays a type error — the user
                /// should cast explicitly in those cases.
                if(!compatible && lhsTy && isNumericScalar(lhsTy)){
                    auto *rhsLit = asNumericLiteral(_expr->rhs);
                    if(canCoerceLiteralTo(rhsLit, lhsTy)){
                        _expr->rhs->resolvedType = lhs_res;
                        rhs_res = lhs_res;
                        compatible = true;
                    }
                }
                if(!compatible && rhsTy && isNumericScalar(rhsTy)){
                    auto *lhsLit = asNumericLiteral(_expr->lhs);
                    if(canCoerceLiteralTo(lhsLit, rhsTy)){
                        _expr->lhs->resolvedType = rhs_res;
                        lhs_res = rhs_res;
                        compatible = true;
                    }
                }
                if(!compatible){
                    auto e = std::make_unique<TypeError>("Failed to match type in binary expr.");
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
            }
            /// Single-level matrix lvalue assignment (`m[col] = vec`) is not
            /// portably representable across all backends — HLSL would need
            /// per-row statement expansion that we don't have infrastructure
            /// for yet. Require the user to write `m[col][row] = …`
            /// per-element. See OmegaSL-Feature-Gap-Survey §12.1.
            if(_expr->op == OP_EQUAL || _expr->op == OP_PLUSEQUAL ||
               _expr->op == OP_MINUSEQUAL || _expr->op == OP_MULEQUAL ||
               _expr->op == OP_DIVEQUAL || _expr->op == OP_MODEQUAL ||
               _expr->op == OP_ANDEQUAL || _expr->op == OP_OREQUAL ||
               _expr->op == OP_XOREQUAL || _expr->op == OP_LSHIFTEQUAL ||
               _expr->op == OP_RSHIFTEQUAL){
                /// §3.6 — reject writes to a `const` local. Walks past
                /// any leading INDEX_EXPRs / member access on the LHS so
                /// that `c[0] = x` and `c.field = x` are caught when `c`
                /// itself is the const binding (the value is immutable
                /// transitively, matching HLSL/MSL/GLSL semantics for
                /// `const` aggregates).
                ast::Expr *lhsRoot = _expr->lhs;
                while(lhsRoot){
                    if(lhsRoot->type == INDEX_EXPR){
                        lhsRoot = ((ast::IndexExpr *)lhsRoot)->lhs;
                    }
                    else if(lhsRoot->type == MEMBER_EXPR){
                        lhsRoot = ((ast::MemberExpr *)lhsRoot)->lhs;
                    }
                    else {
                        break;
                    }
                }
                if(lhsRoot && lhsRoot->type == ID_EXPR){
                    auto *idExpr = (ast::IdExpr *)lhsRoot;
                    auto found = currentContext->variableMap.find(idExpr->id);
                    if(found != currentContext->variableMap.end() && found->second.isConst){
                        auto e = std::make_unique<TypeError>(
                            std::string("Cannot assign to `const` local `") + idExpr->id + "`.");
                        e->loc = _expr->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return nullptr;
                    }
                }
                if(_expr->lhs->type == INDEX_EXPR){
                    auto *idx = (ast::IndexExpr *)_expr->lhs;
                    /// idx->lhs is the matrix variable for a single-level
                    /// write; if it's itself an INDEX_EXPR, this is the
                    /// allowed two-level form `m[col][row] = …`.
                    if(idx->lhs->type != INDEX_EXPR){
                        auto innerTy = resolveTypeWithExpr(idx->lhs->resolvedType);
                        if(innerTy && isMatrixType(innerTy)){
                            auto e = std::make_unique<TypeError>(
                                "Cannot assign to a matrix column; use two-level indexing `m[col][row] = …`.");
                            e->loc = _expr->loc.value_or(ErrorLoc{});
                            diagnostics->addError(std::move(e));
                            return nullptr;
                        }
                    }
                }
            }

            /// Return type inference.
            ///
            /// Relational, equality, and logical operators always
            /// produce `bool` (matches C/HLSL/MSL/GLSL spec). Without
            /// this, `bool flag = (x > 0)` and the ternary condition
            /// (§3.2) both fail to type-check because the binary
            /// expression silently inherits the operand type. Existing
            /// `if (x > 0)` callers don't care — Sema doesn't enforce
            /// a bool constraint on `if` conditions — so this is a
            /// strict semantic improvement.
            const auto &op = _expr->op;
            bool isCompareOp =
                op == OP_LESS || op == OP_LESSEQUAL ||
                op == OP_GREATER || op == OP_GREATEREQUAL ||
                op == OP_ISEQUAL || op == OP_NOTEQUAL ||
                op == OP_LOGAND || op == OP_LOGOR;
            if(isCompareOp){
                return ast::TypeExpr::Create(ast::builtins::bool_type);
            }

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

            /// Propagate resolvedType so type-aware codegen (HLSL matrix
            /// index swap) can ask "is the lhs of this `INDEX_EXPR` itself
            /// an `INDEX_EXPR` whose lhs resolves to a matrix?" — the
            /// rewrite needs to inspect the inner lhs at emit time.
            _expr->lhs->resolvedType = lhs_res;
            _expr->idx_expr->resolvedType = idx_expr_res;

            auto _t = resolveTypeWithExpr(lhs_res);

            /// Array-local indexing: peel one (outermost) dimension.
            /// `float4 tile[16][16]` indexed once is `float4[16]`, twice is
            /// a plain `float4` (which a third `[i]` then component-indexes).
            /// This must precede the vector/matrix checks below: the base
            /// type of an array of vectors is still a vector, but a single
            /// `[i]` selects an array element, not a component.
            if(!lhs_res->arrayDims.empty()){
                auto idxT = resolveTypeWithExpr(idx_expr_res);
                if(idxT != ast::builtins::uint_type && idxT != ast::builtins::int_type){
                    auto e = std::make_unique<TypeError>("Array index must be an int or uint type.");
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
                auto *elemT = ast::TypeExpr::Create(lhs_res->name, lhs_res->pointer);
                for(size_t i = 1; i < lhs_res->arrayDims.size(); i++){
                    elemT->arrayDims.push_back(lhs_res->arrayDims[i]);
                }
                return elemT;
            }

            /// Vector component access: vecN[i] -> scalar, for every vector
            /// family (float / int / uint / half / short / ushort / long /
            /// ulong / bool). Generalized via `vectorComponentInfo` so index
            /// and swizzle stay in lockstep — previously only float/int/uint
            /// were indexable while the MEMBER_EXPR swizzle path already
            /// resolved all families, so e.g. `long2.x` worked but `long2[0]`
            /// errored. (Matrices return `{nullptr,0}` here and fall through
            /// to the matrix-column path below.)
            {
                auto vecInfo = vectorComponentInfo(_t);
                if(vecInfo.scalar){
                    /// The component index must be an integer, matching the
                    /// array- and buffer-index rules above/below. (The
                    /// pre-generalization float/int/uint vector path skipped
                    /// this check; it's added now that indexing is uniform.)
                    auto idxT = resolveTypeWithExpr(idx_expr_res);
                    if(idxT != ast::builtins::uint_type && idxT != ast::builtins::int_type){
                        auto e = std::make_unique<TypeError>("Vector component index must be an int or uint type.");
                        e->loc = _expr->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return nullptr;
                    }
                    return ast::TypeExpr::Create(vecInfo.scalar);
                }
            }

            /// Matrix column access: matNxM[i] -> floatM (column vector)
            if(isMatrixType(_t)){
                /// The column index must be an integer (same rule as array /
                /// buffer / vector indices). The *row* index of the two-level
                /// `m[col][row]` form needs no check here: `m[col]` resolves
                /// to a column vector, so the outer `[row]` is validated by
                /// the vector path above.
                auto idxT = resolveTypeWithExpr(idx_expr_res);
                if(idxT != ast::builtins::uint_type && idxT != ast::builtins::int_type){
                    auto e = std::make_unique<TypeError>("Matrix index must be an int or uint type.");
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
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

            /// §12.2 follow-up — integer matrix column access. The lowering
            /// is an array of column vectors, so `m[col]` is a plain array
            /// index that yields the `intR` / `uintR` column. Same int/uint
            /// index-type rule as the float-matrix path above.
            if(isIntegerMatrixType(_t)){
                auto idxT = resolveTypeWithExpr(idx_expr_res);
                if(idxT != ast::builtins::uint_type && idxT != ast::builtins::int_type){
                    auto e = std::make_unique<TypeError>("Matrix index must be an int or uint type.");
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
                return ast::TypeExpr::Create(integerMatrixColumnVector(_t));
            }

            if(_t != ast::builtins::buffer_type){
                auto e = std::make_unique<TypeError>("Indexing is only supported on buffer, vector, and matrix types."); e->loc = _expr->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
                return nullptr;
            }

            _t = resolveTypeWithExpr(idx_expr_res);
            if(_t != ast::builtins::uint_type && _t != ast::builtins::int_type){
                auto e = std::make_unique<TypeError>("Index of buffer must be an int or uint type."); e->loc = _expr->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
            }

            /// Defense in depth: a well-formed buffer always carries its
            /// element type in `args[0]` (RESOURCE_DECL enforces arity), but
            /// guard the dereference so a malformed buffer can never crash
            /// here — diagnose instead.
            if(lhs_res->args.empty()){
                auto e = std::make_unique<TypeError>("Cannot index buffer with no element type."); e->loc = _expr->loc.value_or(ErrorLoc{}); diagnostics->addError(std::move(e));
                return nullptr;
            }

            return lhs_res->args[0];

        }
        else if(expr->type == CALL_EXPR){
            auto _expr = (ast::CallExpr *)expr;

            assert(_expr->callee->type == ID_EXPR);

            auto _id_expr = (ast::IdExpr *)_expr->callee;

            auto func_found = resolveFuncTypeWithName(_id_expr->id);

            /// §3.5 — overload resolution. When the name resolves to a
            /// non-builtin user FuncType and more than one user
            /// function shares that name, the first-match returned by
            /// `resolveFuncTypeWithName` is the wrong one in the
            /// general case. Re-resolve by signature: walk every arg
            /// to get its type, then pick the candidate whose
            /// parameter list matches exactly. Builtins keep going
            /// through their own dispatch chain below — OmegaSL
            /// doesn't allow user code to overload them.
            if(func_found != nullptr && !func_found->builtin){
                auto candidates = resolveFuncCandidatesByName(_id_expr->id);
                /// Skip the work if there's only one candidate — the
                /// common case stays exactly as fast as before, and
                /// the only behavioral difference is that we now stamp
                /// `resolvedCallee` for codegen.
                if(candidates.size() > 1){
                    OmegaCommon::Vector<ast::TypeExpr *> argTypes;
                    argTypes.reserve(_expr->args.size());
                    bool argSemaFailed = false;
                    for(auto *arg : _expr->args){
                        auto t = performSemForExpr(arg, funcContext);
                        if(!t){ argSemaFailed = true; break; }
                        argTypes.push_back(t);
                    }
                    if(argSemaFailed) return nullptr;
                    auto *resolved = resolveOverload(candidates, argTypes);
                    if(resolved == nullptr){
                        std::string msg = "No matching overload for `";
                        msg += std::string(_id_expr->id);
                        msg += "` with the given argument types.";
                        auto e = std::make_unique<TypeError>(msg);
                        e->loc = _expr->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return nullptr;
                    }
                    func_found = resolved;
                }
                /// Stamp the resolved callee so codegen can pick the
                /// right mangled spelling at the call site (the bare
                /// ID_EXPR is ambiguous for an overloaded name).
                _expr->resolvedCallee = func_found;
            }

            if(func_found == nullptr){
                /// Check if it's a known math intrinsic. Canonicalize the
                /// `mod`/`mad` aliases up front so the dispatch and the
                /// arg-count diagnostics speak the same name the backends
                /// emit (§5.1.0).
                OmegaCommon::StrRef fname = ast::canonicalBuiltinAlias(_id_expr->id);

                /// §5.1.0 — `modf(x, out ip)` / `frexp(x, out e)`. Both
                /// return the fractional part / mantissa (same type as `x`)
                /// and write a second result through an out-param. Validated
                /// here rather than via the generic math buckets because the
                /// out-param's type rule is special:
                ///   modf  — `ip` has the same float type as `x`.
                ///   frexp — `e` is the int vector matching `x`'s arity. (HLSL
                ///           writes a float exponent; the backend casts back to
                ///           the int out-param — see HLSLTarget.)
                if(fname == BUILTIN_MODF || fname == BUILTIN_FREXP){
                    if(_expr->args.size() != 2){
                        auto e = std::make_unique<ArgumentCountMismatch>();
                        e->functionName = fname; e->expected = 2;
                        e->actual = (unsigned)_expr->args.size();
                        e->loc = _expr->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return nullptr;
                    }
                    auto reportErr = [&](const std::string& msg){
                        auto e = std::make_unique<TypeError>(msg);
                        e->loc = _expr->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                    };
                    auto x_e = performSemForExpr(_expr->args[0], funcContext);
                    auto o_e = performSemForExpr(_expr->args[1], funcContext);
                    if(!x_e || !o_e) return nullptr;
                    /// Stamp the resolved arg types so HLSL codegen can read
                    /// `x`'s arity at the call site (mirrors INDEX_EXPR).
                    _expr->args[0]->resolvedType = x_e;
                    _expr->args[1]->resolvedType = o_e;
                    auto xTy = resolveTypeWithExpr(x_e);
                    auto oTy = resolveTypeWithExpr(o_e);
                    if(!xTy || !oTy) return nullptr;

                    using namespace ast::builtins;
                    unsigned arity = (xTy == float_type) ? 1 : (xTy == float2_type) ? 2
                                   : (xTy == float3_type) ? 3 : (xTy == float4_type) ? 4 : 0;
                    if(arity == 0){
                        reportErr("`" + std::string(fname) + "` requires a float / float-vector first argument.");
                        return nullptr;
                    }

                    /// The out-param must be a writable (non-const) lvalue:
                    /// walk past index / member access to the root binding,
                    /// the same shape the §3.6 const-write check uses.
                    ast::Expr *root = _expr->args[1];
                    while(root){
                        if(root->type == INDEX_EXPR) root = ((ast::IndexExpr *)root)->lhs;
                        else if(root->type == MEMBER_EXPR) root = ((ast::MemberExpr *)root)->lhs;
                        else break;
                    }
                    bool writable = root && root->type == ID_EXPR;
                    if(writable){
                        auto found = currentContext->variableMap.find(((ast::IdExpr *)root)->id);
                        if(found != currentContext->variableMap.end() && found->second.isConst)
                            writable = false;
                    }
                    if(!writable){
                        reportErr("2nd argument of `" + std::string(fname) + "` must be a writable (non-const) variable (out-param).");
                        return nullptr;
                    }

                    if(fname == BUILTIN_MODF){
                        if(oTy != xTy){
                            reportErr("`modf` integer-part out-param must have the same type as the first argument.");
                            return nullptr;
                        }
                    } else {
                        ast::Type *wantInt = (arity == 1) ? int_type : (arity == 2) ? int2_type
                                           : (arity == 3) ? int3_type : int4_type;
                        if(oTy != wantInt){
                            reportErr("`frexp` exponent out-param must be int / intN matching the first argument's arity.");
                            return nullptr;
                        }
                    }
                    /// Return type = the fractional part / mantissa, same type as `x`.
                    return ast::TypeExpr::Create(xTy);
                }

                int expectedArgs = -1; // -1 = unknown function
                bool returnsScalar = false; // true for length() which returns scalar from vector

                /// 1-arg intrinsics (same type in, same type out)
                if(fname == BUILTIN_SIN || fname == BUILTIN_COS || fname == BUILTIN_TAN ||
                   fname == BUILTIN_ASIN || fname == BUILTIN_ACOS || fname == BUILTIN_ATAN ||
                   fname == BUILTIN_SQRT || fname == BUILTIN_ABS || fname == BUILTIN_FLOOR ||
                   fname == BUILTIN_CEIL || fname == BUILTIN_ROUND || fname == BUILTIN_FRAC ||
                   fname == BUILTIN_NORMALIZE ||
                   fname == BUILTIN_EXP || fname == BUILTIN_EXP2 ||
                   fname == BUILTIN_LOG || fname == BUILTIN_LOG2 ||
                   fname == BUILTIN_SIGN || fname == BUILTIN_SATURATE ||
                   fname == BUILTIN_TRUNC || fname == BUILTIN_RSQRT ||
                   fname == BUILTIN_DEGREES || fname == BUILTIN_RADIANS ||
                   fname == BUILTIN_SINH || fname == BUILTIN_COSH || fname == BUILTIN_TANH){
                    expectedArgs = 1;
                }
                else if(fname == BUILTIN_LENGTH){
                    expectedArgs = 1;
                    returnsScalar = true;
                }
                /// §5.2 — `distance(a,b)` is the 2-arg analogue of `length`:
                /// it returns the scalar length of `a - b`.
                else if(fname == BUILTIN_DISTANCE){
                    expectedArgs = 2;
                    returnsScalar = true;
                }
                /// 2-arg intrinsics
                else if(fname == BUILTIN_ATAN2 || fname == BUILTIN_POW ||
                        fname == BUILTIN_MIN || fname == BUILTIN_MAX ||
                        fname == BUILTIN_STEP || fname == BUILTIN_REFLECT ||
                        fname == BUILTIN_FMOD || fname == BUILTIN_LDEXP){
                    expectedArgs = 2;
                }
                /// 3-arg intrinsics. §5.2 adds `faceforward(n,i,ng)` and
                /// `refract(i,n,eta)` — both return the first argument's
                /// (vector) type; `refract`'s `eta` is a scalar, which the
                /// loose per-arg validation below already tolerates.
                else if(fname == BUILTIN_CLAMP || fname == BUILTIN_LERP ||
                        fname == BUILTIN_SMOOTHSTEP || fname == BUILTIN_FMA ||
                        fname == BUILTIN_FACEFORWARD || fname == BUILTIN_REFRACT){
                    expectedArgs = 3;
                }

                /// Matrix intrinsics with special return types
                bool isTranspose = (fname == "transpose");
                bool isDeterminant = (fname == "determinant");
                if(isTranspose || isDeterminant){
                    expectedArgs = 1;
                }
                /// §5.2 — `inverse(m)` returns the same square-matrix type.
                /// Non-square / non-matrix arguments are rejected below.
                bool isInverse = (fname == BUILTIN_INVERSE);
                if(isInverse){
                    expectedArgs = 1;
                }

                /// §5.2 — `any(v)` / `all(v)` reduce a bool vector to a
                /// scalar bool (custom return type, handled below).
                bool isBoolReduce = (fname == BUILTIN_ANY || fname == BUILTIN_ALL);
                if(isBoolReduce){
                    expectedArgs = 1;
                }
                /// §5.2 — component-wise comparison of two same-type numeric
                /// vectors, producing the matching bool vector.
                bool isVecCompare =
                    fname == BUILTIN_LESSTHAN || fname == BUILTIN_LESSTHANEQUAL ||
                    fname == BUILTIN_GREATERTHAN || fname == BUILTIN_GREATERTHANEQUAL ||
                    fname == BUILTIN_EQUAL || fname == BUILTIN_NOTEQUAL;
                if(isVecCompare){
                    expectedArgs = 2;
                }

                /// §6.2 — compute barriers: 0-arg, void return, compute-only.
                bool isBarrier = (fname == BUILTIN_THREADGROUP_BARRIER || fname == BUILTIN_DEVICE_BARRIER);
                if(isBarrier){
                    expectedArgs = 0;
                    bool inCompute = funcContext && funcContext->type == SHADER_DECL
                        && ((ast::ShaderDecl *)funcContext)->shaderType == ast::ShaderDecl::Compute;
                    if(!inCompute){
                        auto e = std::make_unique<InvalidAttribute>(
                            std::string("`") + std::string(fname) + "` is only valid inside a compute shader");
                        e->loc = _expr->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return nullptr;
                    }
                }

                /// `>= 0` (not `> 0`) so the 0-arg barriers get their arg
                /// count enforced too; -1 (unknown function) still skips.
                if(expectedArgs >= 0 && (int)_expr->args.size() != expectedArgs){
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
                ast::TypeExpr *secondArgType = nullptr;
                for(unsigned i = 0; i < _expr->args.size(); i++){
                    auto argType = performSemForExpr(_expr->args[i],funcContext);
                    if(!argType) return nullptr;
                    if(i == 0) firstArgType = argType;
                    else if(i == 1) secondArgType = argType;
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
                if(isInverse && firstArgType){
                    /// Stamp the resolved arg type so codegen can read the
                    /// matrix size at the call site (the generic bucket below
                    /// doesn't, and `performSemForExpr` only stamps some expr
                    /// kinds — an `inverse(a * b)` binary-expr arg would
                    /// otherwise reach the HLSL/MSL lowering with a null
                    /// `resolvedType`). Mirrors the modf / frexp path.
                    _expr->args[0]->resolvedType = firstArgType;
                    /// Square matrices only — inverse is undefined otherwise.
                    auto argTy = resolveTypeWithExpr(firstArgType);
                    if(argTy == ast::builtins::float2x2_type ||
                       argTy == ast::builtins::float3x3_type ||
                       argTy == ast::builtins::float4x4_type){
                        return firstArgType;
                    }
                    auto e = std::make_unique<TypeError>(
                        "`inverse` requires a square matrix argument (float2x2, float3x3, or float4x4)");
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
                /// §5.2 — any/all + component-wise compare have custom
                /// return types and reject malformed operands. (The shared
                /// `reportTypeErr` lambda is declared further down in this
                /// function, after the math-dispatch block, so use a local.)
                auto reportBoolErr = [&](const std::string &msg){
                    auto e = std::make_unique<TypeError>(msg);
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                };
                /// `any`/`all`: the argument must be a bool vector
                /// (bool2/3/4). A scalar `bool` or a numeric vector is
                /// rejected — GLSL's `any`/`all` accept only `bvec`, so
                /// the strict rule keeps the surface portable.
                if(isBoolReduce && firstArgType){
                    auto argTy = resolveTypeWithExpr(firstArgType);
                    if(argTy != ast::builtins::bool2_type &&
                       argTy != ast::builtins::bool3_type &&
                       argTy != ast::builtins::bool4_type){
                        reportBoolErr("`" + std::string(fname) + "` requires a bool-vector argument (bool2, bool3, or bool4). Build one with a component-wise comparison (lessThan, equal, …) or make_boolN.");
                        return nullptr;
                    }
                    return ast::TypeExpr::Create(ast::builtins::bool_type);
                }
                /// component-wise comparison: both operands must be the
                /// *same* numeric vector type; the result is the bool
                /// vector of matching arity. Bool-vector operands are
                /// rejected (ordered compares are nonsensical on bools, and
                /// the OmegaSL surface keeps `equal`/`notEqual` numeric-only
                /// for one uniform rule).
                if(isVecCompare && firstArgType && secondArgType){
                    auto a0 = resolveTypeWithExpr(firstArgType);
                    auto a1 = resolveTypeWithExpr(secondArgType);
                    auto info = vectorComponentInfo(a0);
                    if(!info.scalar || info.scalar == ast::builtins::bool_type){
                        reportBoolErr("`" + std::string(fname) + "` requires numeric-vector operands (floatN / intN / uintN / …).");
                        return nullptr;
                    }
                    if(a0 != a1){
                        reportBoolErr("`" + std::string(fname) + "` requires both operands to be the same vector type.");
                        return nullptr;
                    }
                    return ast::TypeExpr::Create(vectorTypeForScalarArity(ast::builtins::bool_type, info.arity));
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

                if(!(_t == ast::builtins::sampler1d_type || _t == ast::builtins::sampler2d_type
                     || _t == ast::builtins::sampler3d_type || _t == ast::builtins::samplercube_type)){
                    reportTypeErr("1st param of function " + std::string(BUILTIN_SAMPLE) + " must be a sampler.");
                    return nullptr;
                }

                auto _first_t = _t;
                _t = resolveTypeWithExpr(second_t_e);
                if(_t == nullptr){
                    return nullptr;
                }
                auto _tex_t = _t;

                /// Multisample textures cannot be sampled — they need an
                /// explicit sample index and go through `read`.
                if(_tex_t == ast::builtins::texture2d_ms_type
                   || _tex_t == ast::builtins::texture2d_ms_array_type){
                    reportTypeErr(std::string("Multisample textures cannot be sampled; use `read(tex, coord, sample_index)` instead."));
                    return nullptr;
                }

                ///sampler1d ↔ texture1d / texture1d_array
                if(_first_t == ast::builtins::sampler1d_type){

                    if(_tex_t != ast::builtins::texture1d_type
                       && _tex_t != ast::builtins::texture1d_array_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_SAMPLE) + " must be a texture1d or texture1d_array (paired with sampler1d)");
                        return nullptr;
                    }

                    _t = resolveTypeWithExpr(third_t_e);
                    if(_t == nullptr){
                        return nullptr;
                    }

                    if(_tex_t == ast::builtins::texture1d_type){
                        if(_t != ast::builtins::float_type){
                            reportTypeErr("3rd param of function " + std::string(BUILTIN_SAMPLE) + " must be a float for texture1d");
                            return nullptr;
                        }
                    } else {
                        /// texture1d_array — coord = float2 (u + layer)
                        if(_t != ast::builtins::float2_type){
                            reportTypeErr("3rd param of function " + std::string(BUILTIN_SAMPLE) + " must be a float2 (u, layer) for texture1d_array");
                            return nullptr;
                        }
                    }

                }
                ///sampler2d ↔ texture2d / texture2d_array
                else if(_first_t == ast::builtins::sampler2d_type){

                    if(_tex_t != ast::builtins::texture2d_type
                       && _tex_t != ast::builtins::texture2d_array_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_SAMPLE) + " must be a texture2d or texture2d_array (paired with sampler2d)");
                        return nullptr;
                    }

                    _t = resolveTypeWithExpr(third_t_e);
                    if(_t == nullptr){
                        return nullptr;
                    }

                    if(_tex_t == ast::builtins::texture2d_type){
                        if(_t != ast::builtins::float2_type){
                            reportTypeErr("3rd param of function " + std::string(BUILTIN_SAMPLE) + " must be a float2 for texture2d");
                            return nullptr;
                        }
                    } else {
                        /// texture2d_array — coord = float3 (uv + layer)
                        if(_t != ast::builtins::float3_type){
                            reportTypeErr("3rd param of function " + std::string(BUILTIN_SAMPLE) + " must be a float3 (uv, layer) for texture2d_array");
                            return nullptr;
                        }
                    }
                }
                ///sampler3d ↔ texture3d
                else if(_first_t == ast::builtins::sampler3d_type) {
                    if(_tex_t != ast::builtins::texture3d_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_SAMPLE) + " must be a texture3d (paired with sampler3d)");
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
                ///samplercube ↔ texturecube / texturecube_array
                else if(_first_t == ast::builtins::samplercube_type){
                    if(_tex_t != ast::builtins::texturecube_type
                       && _tex_t != ast::builtins::texturecube_array_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_SAMPLE) + " must be a texturecube or texturecube_array (paired with samplercube)");
                        return nullptr;
                    }

                    _t = resolveTypeWithExpr(third_t_e);
                    if(_t == nullptr){
                        return nullptr;
                    }

                    if(_tex_t == ast::builtins::texturecube_type){
                        /// texturecube — coord = float3 direction vector
                        if(_t != ast::builtins::float3_type){
                            reportTypeErr("3rd param of function " + std::string(BUILTIN_SAMPLE) + " must be a float3 (direction) for texturecube");
                            return nullptr;
                        }
                    } else {
                        /// texturecube_array — coord = float4 (xyz dir + w layer)
                        if(_t != ast::builtins::float4_type){
                            reportTypeErr("3rd param of function " + std::string(BUILTIN_SAMPLE) + " must be a float4 (direction, layer) for texturecube_array");
                            return nullptr;
                        }
                    }
                }
            }
            /// @brief sampleLOD(s,t,c,lod) / sampleBias(s,t,c,bias) — explicit
            /// LOD or LOD-bias sampling. The (sampler, texture, coord) triple
            /// follows the same pairing rules as `sample`; the trailing scalar
            /// is always `float`.
            else if(func_found == ast::builtins::sampleLOD
                    || func_found == ast::builtins::sampleBias){
                OmegaCommon::StrRef name = (func_found == ast::builtins::sampleLOD)
                    ? OmegaCommon::StrRef(BUILTIN_SAMPLE_LOD)
                    : OmegaCommon::StrRef(BUILTIN_SAMPLE_BIAS);
                const char *trailingArgName = (func_found == ast::builtins::sampleLOD) ? "lod" : "bias";

                if(_expr->args.size() != 4){
                    auto e = std::make_unique<ArgumentCountMismatch>();
                    e->functionName = name; e->expected = 4;
                    e->actual = (unsigned)_expr->args.size();
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                if(!validateSampleTriple(_expr, name, funcContext, nullptr, nullptr)) return nullptr;

                auto fourth_t_e = performSemForExpr(_expr->args[3], funcContext);
                if(!fourth_t_e) return nullptr;
                auto trailTy = resolveTypeWithExpr(fourth_t_e);
                if(trailTy != ast::builtins::float_type){
                    reportTypeErr("4th param of function " + std::string(name) + " (" + trailingArgName + ") must be a float.");
                    return nullptr;
                }
            }
            /// @brief sampleGrad(s,t,c,ddxArg,ddyArg) — gradient-based sampling.
            /// ddx/ddy rank matches the sampler's spatial domain (1D→float,
            /// 2D→float2, 3D and Cube→float3).
            else if(func_found == ast::builtins::sampleGrad){
                if(_expr->args.size() != 5){
                    auto e = std::make_unique<ArgumentCountMismatch>();
                    e->functionName = BUILTIN_SAMPLE_GRAD; e->expected = 5;
                    e->actual = (unsigned)_expr->args.size();
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                ast::Type *samplerTy = nullptr;
                if(!validateSampleTriple(_expr, BUILTIN_SAMPLE_GRAD, funcContext, &samplerTy, nullptr)) return nullptr;
                auto expectedGrad = gradientTypeForSampler(samplerTy);
                if(!expectedGrad){
                    /// validateSampleTriple already accepted the sampler — this
                    /// is unreachable, but bail rather than crash.
                    return nullptr;
                }

                auto ddx_e = performSemForExpr(_expr->args[3], funcContext);
                auto ddy_e = performSemForExpr(_expr->args[4], funcContext);
                if(!ddx_e || !ddy_e) return nullptr;
                if(resolveTypeWithExpr(ddx_e) != expectedGrad){
                    reportTypeErr("4th param of function " + std::string(BUILTIN_SAMPLE_GRAD) + " (ddx) does not match the expected gradient shape for the given sampler.");
                    return nullptr;
                }
                if(resolveTypeWithExpr(ddy_e) != expectedGrad){
                    reportTypeErr("5th param of function " + std::string(BUILTIN_SAMPLE_GRAD) + " (ddy) does not match the expected gradient shape for the given sampler.");
                    return nullptr;
                }
            }
            /// @brief gather(s,t,c) and gather{Red,Green,Blue,Alpha}(s,t,c).
            /// Only valid on 2D / 2D-array / cube / cube-array textures —
            /// every backend's gather instruction has that domain.
            else if(func_found == ast::builtins::gather
                    || func_found == ast::builtins::gatherRed
                    || func_found == ast::builtins::gatherGreen
                    || func_found == ast::builtins::gatherBlue
                    || func_found == ast::builtins::gatherAlpha){
                OmegaCommon::StrRef name = func_found->name;

                if(_expr->args.size() != 3){
                    auto e = std::make_unique<ArgumentCountMismatch>();
                    e->functionName = name; e->expected = 3;
                    e->actual = (unsigned)_expr->args.size();
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                ast::Type *texTy = nullptr;
                if(!validateSampleTriple(_expr, name, funcContext, nullptr, &texTy)) return nullptr;
                if(!isGatherableTextureType(texTy)){
                    reportTypeErr(std::string(name) + " is only valid on texture2d, texture2d_array, texturecube, or texturecube_array.");
                    return nullptr;
                }
            }
            /// @brief calculateLOD(sampler, texture, coord) — query the LOD
            /// the hardware would select for `coord`. Returns a scalar
            /// `float`. The (sampler, texture, coord) triple follows the same
            /// pairing rules as `sample`; multisample textures are rejected by
            /// validateSampleTriple. Like `sample`/`sampleBias`, this relies on
            /// fragment-stage derivatives in practice, but stage enforcement is
            /// deferred to match the existing `sample` precedent.
            else if(func_found == ast::builtins::calculateLOD){
                if(_expr->args.size() != 3){
                    auto e = std::make_unique<ArgumentCountMismatch>();
                    e->functionName = BUILTIN_CALCULATE_LOD; e->expected = 3;
                    e->actual = (unsigned)_expr->args.size();
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
                ast::Type *texTy = nullptr;
                if(!validateSampleTriple(_expr, BUILTIN_CALCULATE_LOD, funcContext, nullptr, &texTy)) return nullptr;
                /// 1D textures have no mip pyramid; Metal exposes no
                /// `calculate_*_lod` for `texture1d`, so reject 1D here for
                /// portability (HLSL/GLSL do have it, but the query is
                /// degenerate — LOD is always 0). Revisit behind
                /// TEXTURE1D_MIP_SAMPLE if a real workload needs it.
                if(texTy == ast::builtins::texture1d_type
                   || texTy == ast::builtins::texture1d_array_type){
                    reportTypeErr("`calculateLOD` is not supported on 1D textures (no mip pyramid; Metal has no LOD query for `texture1d`).");
                    return nullptr;
                }
                /// returns float — falls through to func_found->returnType.
            }
            /// @brief getDimensions(texture, lod) — query the mip-level
            /// dimensions. `lod` is required (pass `0` for the base level).
            /// The return shape depends on the texture's spatial rank:
            /// `uint` (1D), `uint2` (1D-array / 2D / cube), `uint3` (2D-array
            /// / 3D / cube-array). Synthesized per-call (like `transpose`)
            /// rather than read from the FuncType's placeholder return type.
            /// Multisample textures are rejected — the per-backend dimension
            /// query for MS is asymmetric and deferred.
            else if(func_found == ast::builtins::getDimensions){
                if(_expr->args.size() != 2){
                    auto e = std::make_unique<ArgumentCountMismatch>();
                    e->functionName = BUILTIN_GET_DIMENSIONS; e->expected = 2;
                    e->actual = (unsigned)_expr->args.size();
                    e->loc = _expr->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                auto tex_e = performSemForExpr(_expr->args[0], funcContext);
                auto lod_e = performSemForExpr(_expr->args[1], funcContext);
                if(!tex_e || !lod_e) return nullptr;
                auto texTy = resolveTypeWithExpr(tex_e);
                auto lodTy = resolveTypeWithExpr(lod_e);
                if(!texTy || !lodTy) return nullptr;

                if(isMSTextureType(texTy)){
                    reportTypeErr("`getDimensions` on multisample textures is not supported yet (the per-backend dimension query for multisample textures is asymmetric).");
                    return nullptr;
                }
                if(lodTy != ast::builtins::int_type && lodTy != ast::builtins::uint_type){
                    reportTypeErr("2nd param of function " + std::string(BUILTIN_GET_DIMENSIONS) + " (lod) must be an int or uint.");
                    return nullptr;
                }

                ast::Type *retTy = nullptr;
                if(texTy == ast::builtins::texture1d_type){
                    retTy = ast::builtins::uint_type;
                } else if(texTy == ast::builtins::texture1d_array_type
                          || texTy == ast::builtins::texture2d_type
                          || texTy == ast::builtins::texturecube_type){
                    retTy = ast::builtins::uint2_type;
                } else if(texTy == ast::builtins::texture2d_array_type
                          || texTy == ast::builtins::texture3d_type
                          || texTy == ast::builtins::texturecube_array_type){
                    retTy = ast::builtins::uint3_type;
                } else {
                    reportTypeErr("1st param of function " + std::string(BUILTIN_GET_DIMENSIONS) + " must be a (non-multisample) texture type.");
                    return nullptr;
                }
                return ast::TypeExpr::Create(retTy);
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

                if(_t == ast::builtins::texture2d_ms_type
                   || _t == ast::builtins::texture2d_ms_array_type){
                    reportTypeErr(std::string("Multisample textures are not writable from a shader."));
                    return nullptr;
                }

                if(_t == ast::builtins::texture1d_type){

                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr){
                        return nullptr;
                    }

                    if(_t != ast::builtins::int_type && _t != ast::builtins::uint_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_WRITE) + " must be an int or uint for texture1d");
                        return nullptr;
                    }

                }
                else if(_t == ast::builtins::texture1d_array_type){
                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr) return nullptr;
                    if(_t != ast::builtins::int2_type && _t != ast::builtins::uint2_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_WRITE) + " must be an int2 or uint2 (u, layer) for texture1d_array");
                        return nullptr;
                    }
                }
                else if(_t == ast::builtins::texture2d_type){
                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr){
                        return nullptr;
                    }

                    if(_t != ast::builtins::int2_type && _t != ast::builtins::uint2_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_WRITE) + " must be an int2 or uint2 for texture2d");
                        return nullptr;
                    }
                }
                else if(_t == ast::builtins::texture2d_array_type){
                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr) return nullptr;
                    if(_t != ast::builtins::int3_type && _t != ast::builtins::uint3_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_WRITE) + " must be an int3 or uint3 (uv, layer) for texture2d_array");
                        return nullptr;
                    }
                }
                else if(_t == ast::builtins::texture3d_type){
                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr){
                        return nullptr;
                    }

                    if(_t != ast::builtins::int3_type && _t != ast::builtins::uint3_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_WRITE) + " must be an int3 or uint3 for texture3d");
                        return nullptr;
                    }
                }
                else if(_t == ast::builtins::texturecube_type
                        || _t == ast::builtins::texturecube_array_type){
                    /// Cube write is deferred. HLSL supports it via Texture2DArray
                    /// aliasing on the underlying resource, but Metal exposes only
                    /// `texturecube<...,access::write>` and GLSL has no `imageCube`
                    /// write surface that lines up cleanly with the others. Re-evaluate
                    /// when the runtime cube path lands.
                    reportTypeErr(std::string("`write` to cube textures is not supported yet (compile-path-only). See OmegaSL-Reference.md §2."));
                    return nullptr;
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
                /// @brief read(texture texture, coord [, sample_index]) function.
                /// Plain textures take 2 args (texture + integer coord). Multisample
                /// textures additionally take a `uint` sample index as the 3rd arg.
            else if(func_found == ast::builtins::read){

                if(_expr->args.size() < 2 || _expr->args.size() > 3){
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
                auto _tex_t = _t;

                bool isMS = (_tex_t == ast::builtins::texture2d_ms_type
                             || _tex_t == ast::builtins::texture2d_ms_array_type);

                if(isMS && _expr->args.size() != 3){
                    reportTypeErr(std::string("Multisample `read` requires 3 arguments: texture, coord, sample_index."));
                    return nullptr;
                }
                if(!isMS && _expr->args.size() != 2){
                    reportTypeErr(std::string("Non-multisample `read` takes 2 arguments: texture, coord."));
                    return nullptr;
                }

                if(_tex_t == ast::builtins::texture1d_type){
                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr) return nullptr;
                    if(_t != ast::builtins::int_type && _t != ast::builtins::uint_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_READ) + " must be an int or uint for texture1d");
                        return nullptr;
                    }
                }
                else if(_tex_t == ast::builtins::texture1d_array_type){
                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr) return nullptr;
                    if(_t != ast::builtins::int2_type && _t != ast::builtins::uint2_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_READ) + " must be an int2 or uint2 (u, layer) for texture1d_array");
                        return nullptr;
                    }
                }
                else if(_tex_t == ast::builtins::texture2d_type){
                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr) return nullptr;
                    if(_t != ast::builtins::int2_type && _t != ast::builtins::uint2_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_READ) + " must be an int2 or uint2 for texture2d");
                        return nullptr;
                    }
                }
                else if(_tex_t == ast::builtins::texture2d_array_type){
                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr) return nullptr;
                    if(_t != ast::builtins::int3_type && _t != ast::builtins::uint3_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_READ) + " must be an int3 or uint3 (uv, layer) for texture2d_array");
                        return nullptr;
                    }
                }
                else if(_tex_t == ast::builtins::texture2d_ms_type){
                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr) return nullptr;
                    if(_t != ast::builtins::int2_type && _t != ast::builtins::uint2_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_READ) + " must be an int2 or uint2 for texture2d_ms");
                        return nullptr;
                    }
                }
                else if(_tex_t == ast::builtins::texture2d_ms_array_type){
                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr) return nullptr;
                    if(_t != ast::builtins::int3_type && _t != ast::builtins::uint3_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_READ) + " must be an int3 or uint3 (uv, layer) for texture2d_ms_array");
                        return nullptr;
                    }
                }
                else if(_tex_t == ast::builtins::texture3d_type){
                    _t = resolveTypeWithExpr(second_t_e);
                    if(_t == nullptr) return nullptr;
                    if(_t != ast::builtins::int3_type && _t != ast::builtins::uint3_type){
                        reportTypeErr("2nd param of function " + std::string(BUILTIN_READ) + " must be an int3 or uint3 for texture3d");
                        return nullptr;
                    }
                }
                else if(_tex_t == ast::builtins::texturecube_type
                        || _tex_t == ast::builtins::texturecube_array_type){
                    /// Cube load is deferred. HLSL has `TextureCube::Load`-like
                    /// behaviour via `[]` on TextureCubeArray, MSL has no direct
                    /// `texturecube::read`, GLSL `texelFetch` doesn't accept
                    /// samplerCube — backend-asymmetric. See §2.1 of the gap
                    /// survey for the deferral rationale.
                    reportTypeErr(std::string("`read` from cube textures is not supported yet (compile-path-only). See OmegaSL-Reference.md §2."));
                    return nullptr;
                }
                else {
                    reportTypeErr(std::string(BUILTIN_READ) + " expects a texture-type for the first argument.");
                    return nullptr;
                }

                if(isMS){
                    /// 3rd arg: sample index (uint)
                    auto third_t_e = performSemForExpr(_expr->args[2],funcContext);
                    if(third_t_e == nullptr) return nullptr;
                    auto _sidx_t = resolveTypeWithExpr(third_t_e);
                    if(_sidx_t == nullptr) return nullptr;
                    if(_sidx_t != ast::builtins::uint_type && _sidx_t != ast::builtins::int_type){
                        reportTypeErr("3rd param of function " + std::string(BUILTIN_READ) + " (sample index) must be a uint or int");
                        return nullptr;
                    }
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
        else if(expr->type == TERNARY_EXPR){
            /// §3.2 — `cond ? thenExpr : elseExpr`.
            ///
            /// Rules:
            ///   - condition must resolve to `bool`. HLSL/MSL/GLSL all
            ///     accept implicit truth conversions on numeric scalars,
            ///     but the spelling drift between backends is real
            ///     (vector ternary on HLSL is select-style; GLSL has
            ///     no vector ternary at all). Locking the condition to
            ///     `bool` keeps the surface portable — callers write
            ///     `x != 0 ? a : b` for a numeric-style truth check.
            ///   - both branches must produce the same type. A literal
            ///     branch coerces to the other branch's type when the
            ///     literal-coercion rule applies (e.g. `cond ? 0 : v`
            ///     where `v` is a `float`).
            auto *_expr = (ast::TernaryExpr *)expr;
            auto condTy = performSemForExpr(_expr->condition, funcContext);
            if(condTy == nullptr) return nullptr;
            auto condResolved = resolveTypeWithExpr(condTy);
            if(condResolved != ast::builtins::bool_type){
                auto e = std::make_unique<TypeError>(
                    "Ternary condition must resolve to `bool` (got `" + condTy->name + "`)");
                e->loc = _expr->condition->loc.value_or(_expr->loc.value_or(ErrorLoc{}));
                diagnostics->addError(std::move(e));
                return nullptr;
            }

            auto thenTy = performSemForExpr(_expr->thenExpr, funcContext);
            if(thenTy == nullptr) return nullptr;
            auto elseTy = performSemForExpr(_expr->elseExpr, funcContext);
            if(elseTy == nullptr) return nullptr;

            auto thenResolved = resolveTypeWithExpr(thenTy);
            auto elseResolved = resolveTypeWithExpr(elseTy);

            if(thenResolved == elseResolved){
                _expr->resolvedType = thenTy;
                return thenTy;
            }

            /// Asymmetric literal coercion: if one branch is a numeric
            /// literal that fits the other's slot, take the typed
            /// branch's type. Two int literals (already-equal types
            /// hit the fast path above), so we only need the mixed
            /// case.
            if(isNumericScalar(thenResolved) || isNumericScalar(elseResolved)){
                auto *thenLit = asNumericLiteral(_expr->thenExpr);
                auto *elseLit = asNumericLiteral(_expr->elseExpr);
                if(elseLit && canCoerceLiteralTo(elseLit, thenResolved)){
                    _expr->elseExpr->resolvedType = thenTy;
                    _expr->resolvedType = thenTy;
                    return thenTy;
                }
                if(thenLit && canCoerceLiteralTo(thenLit, elseResolved)){
                    _expr->thenExpr->resolvedType = elseTy;
                    _expr->resolvedType = elseTy;
                    return elseTy;
                }
            }

            auto e = std::make_unique<TypeError>(
                "Ternary branches must have the same type (`" + thenTy->name +
                "` vs `" + elseTy->name + "`)");
            e->loc = _expr->loc.value_or(ErrorLoc{});
            diagnostics->addError(std::move(e));
            return nullptr;
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
            bool blockFailed = false;
            for(auto s : block.body){
                auto errBefore = diagnostics->getErrorCount();
                auto res = performSemForStmt(s,funcContext);
                if(!res){
                    /// Keep checking the remaining statements so independent
                    /// errors in one function body all surface in a single
                    /// compile. The failing statement normally added its own
                    /// precise diagnostic; only fall back to a generic one if
                    /// it somehow didn't, so the compile still fails loudly
                    /// (a silent sem failure would emit a broken library).
                    blockFailed = true;
                    if(diagnostics->getErrorCount() == errBefore){
                        auto e = std::make_unique<TypeError>("Failed to perform sem on block statement");
                        diagnostics->addError(std::move(e));
                    }
                    if(diagnostics->getErrorCount() >= DiagnosticEngine::kMaxErrorsBeforeStop) break;
                    continue;
                }

                if(s->type == RETURN_DECL){
                    allTypes.push_back(res);
                    hasReturn = true;
                }
            }

            /// Signal failure to the caller only after every statement has
            /// been checked. Skip the return-type consistency check below —
            /// some statements were abandoned, so it would be spurious.
            if(blockFailed) return nullptr;

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
                /// §1.7 — feature bits implied by field attributes (e.g.
                /// `CullDistance` → CULL_DISTANCE). Stamped onto the resolved
                /// struct Type so the FeatureScanner trips the gate when a
                /// shader's output struct carries the attribute.
                uint64_t structImplicitBits = 0;

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

                    /// Internal structs are shader I/O: their fields are
                    /// interstage varyings or fragment-output targets, so each
                    /// must carry a semantic. HLSL matches stages by semantic
                    /// and Metal by `[[stage_in]]` attribute — neither can
                    /// express a field with no semantic, so an unattributed
                    /// field would compile only on the GLSL/Vulkan backend
                    /// (which auto-assigns `layout(location)` positionally) and
                    /// be rejected by DirectX/Metal. Require the attribute up
                    /// front so the rule is uniform across all three backends.
                    if(isInternal && !f.attributeName.has_value()){
                        auto e = std::make_unique<InvalidAttribute>(std::string("Field `") + f.name + "` in internal struct `" + _decl->name + "` must have a semantic attribute (e.g. `: Position`, `: Color`, `: TexCoord`). Internal structs are shader I/O; HLSL and Metal require a semantic on every field.");
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

                        /// Per-attribute type / index rules.
                        if(f.attributeName.value() == ATTRIBUTE_DEPTH){
                            if(field_ty != ast::builtins::float_type){
                                auto e = std::make_unique<TypeError>(std::string("Attribute `") + ATTRIBUTE_DEPTH + "` requires a `float` field, not `" + field_ty->name + "`");
                                e->loc = _decl->loc.value_or(ErrorLoc{});
                                diagnostics->addError(std::move(e));
                                return false;
                            }
                            if(f.attributeIndex.has_value()){
                                auto e = std::make_unique<InvalidAttribute>(std::string("Attribute `") + ATTRIBUTE_DEPTH + "` does not take an index");
                                e->loc = _decl->loc.value_or(ErrorLoc{});
                                diagnostics->addError(std::move(e));
                                return false;
                            }
                        }
                        else if(f.attributeName.value() == ATTRIBUTE_COLOR){
                            if(field_ty != ast::builtins::float4_type){
                                auto e = std::make_unique<TypeError>(std::string("Attribute `") + ATTRIBUTE_COLOR + "` requires a `float4` field, not `" + field_ty->name + "`");
                                e->loc = _decl->loc.value_or(ErrorLoc{});
                                diagnostics->addError(std::move(e));
                                return false;
                            }
                        }
                        else if(f.attributeName.value() == ATTRIBUTE_OUTPUT_COVERAGE){
                            if(field_ty != ast::builtins::uint_type){
                                auto e = std::make_unique<TypeError>(std::string("Attribute `") + ATTRIBUTE_OUTPUT_COVERAGE + "` requires a `uint` field, not `" + field_ty->name + "`");
                                e->loc = _decl->loc.value_or(ErrorLoc{});
                                diagnostics->addError(std::move(e));
                                return false;
                            }
                            if(f.attributeIndex.has_value()){
                                auto e = std::make_unique<InvalidAttribute>(std::string("Attribute `") + ATTRIBUTE_OUTPUT_COVERAGE + "` does not take an index");
                                e->loc = _decl->loc.value_or(ErrorLoc{});
                                diagnostics->addError(std::move(e));
                                return false;
                            }
                        }
                        else if(f.attributeName.value() == ATTRIBUTE_CLIP_DISTANCE
                                || f.attributeName.value() == ATTRIBUTE_CULL_DISTANCE){
                            /// §1.7 — clip/cull distance: a `float` scalar or a
                            /// `float[N]` array (the planes). No index — the
                            /// array carries all distances (HLSL has only two
                            /// SV_ClipDistance registers, so per-plane indices
                            /// don't map; the array does).
                            if(field_ty != ast::builtins::float_type){
                                auto e = std::make_unique<TypeError>(std::string("Attribute `") + f.attributeName.value() + "` requires a `float` scalar or array field, not `" + field_ty->name + "`");
                                e->loc = _decl->loc.value_or(ErrorLoc{});
                                diagnostics->addError(std::move(e));
                                return false;
                            }
                            if(f.attributeIndex.has_value()){
                                auto e = std::make_unique<InvalidAttribute>(std::string("Attribute `") + f.attributeName.value() + "` does not take an index — use a float array (e.g. `float clip[2] : ClipDistance`)");
                                e->loc = _decl->loc.value_or(ErrorLoc{});
                                diagnostics->addError(std::move(e));
                                return false;
                            }
                            if(f.typeExpr->arrayDims.size() > 1){
                                auto e = std::make_unique<TypeError>(std::string("Attribute `") + f.attributeName.value() + "` field may have at most one array dimension");
                                e->loc = _decl->loc.value_or(ErrorLoc{});
                                diagnostics->addError(std::move(e));
                                return false;
                            }
                            /// §1.7 — CullDistance has no Metal equivalent; flag
                            /// the bit so the shader stubs on MSL and the
                            /// portability scanner warns on undeclared use.
                            if(f.attributeName.value() == ATTRIBUTE_CULL_DISTANCE){
                                structImplicitBits |= OMEGASL_FEATURE_BIT_CULL_DISTANCE;
                            }
                        }
                        else if(f.attributeIndex.has_value()){
                            /// Only `Color(N)` accepts an index today.
                            auto e = std::make_unique<InvalidAttribute>(std::string("Attribute `") + f.attributeName.value() + "` does not take an index");
                            e->loc = _decl->loc.value_or(ErrorLoc{});
                            diagnostics->addError(std::move(e));
                            return false;
                        }
                    }

                    /// §1.6 — an integer interstage varying cannot be
                    /// interpolated; HLSL (`nointerpolation`) and GLSL (`flat`)
                    /// both reject a non-flat integer varying. Catch it at the
                    /// OmegaSL level with a precise diagnostic instead of letting
                    /// the backend compiler emit a cryptic one. Only true
                    /// interstage varyings are affected — system-value / output
                    /// semantics (Position, Depth, OutputCoverage, indexed
                    /// Color(N)) are not interpolated and are exempt.
                    if(isInternal && f.interp == ast::AttributedFieldDecl::Default
                       && isIntegerScalarOrVector(field_ty) && f.attributeName.has_value()){
                        const auto &an = f.attributeName.value();
                        bool isInterpolatedVarying =
                            !(an == ATTRIBUTE_POSITION || an == ATTRIBUTE_DEPTH
                              || an == ATTRIBUTE_OUTPUT_COVERAGE
                              || (an == ATTRIBUTE_COLOR && f.attributeIndex.has_value()));
                        if(isInterpolatedVarying){
                            auto e = std::make_unique<TypeError>(std::string("Integer varying `") + f.name + "` in internal struct `" + _decl->name + "` must be declared `flat` — integers cannot be interpolated.");
                            e->loc = _decl->loc.value_or(ErrorLoc{});
                            diagnostics->addError(std::move(e));
                            return false;
                        }
                    }

                    field_types.insert(std::make_pair(f.name,f.typeExpr));

                }

                /// 3. If all of the above checks succeed, add struct type to TypeMap.
                addTypeToCurrentContext(_decl->name,_decl->scope,field_types);
                /// §1.7 — record structs with a `CullDistance` field so a shader
                /// returning one trips OMEGASL_FEATURE_BIT_CULL_DISTANCE (see
                /// SHADER_DECL). The resolved Type can't carry this (it drops
                /// attribute metadata), and the FeatureScanner only unions
                /// `usedFeatures`, so flagging it from Sema survives the scan.
                if(structImplicitBits & OMEGASL_FEATURE_BIT_CULL_DISTANCE){
                    currentContext->cullDistanceStructs.push_back(_decl->name);
                }
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
                && ty != ast::builtins::uniform_type
                && ty != ast::builtins::texture1d_type
                && ty != ast::builtins::texture2d_type
                && ty != ast::builtins::texture3d_type
                && ty != ast::builtins::texture1d_array_type
                && ty != ast::builtins::texture2d_array_type
                && ty != ast::builtins::texturecube_type
                && ty != ast::builtins::texturecube_array_type
                && ty != ast::builtins::texture2d_ms_type
                && ty != ast::builtins::texture2d_ms_array_type
                && ty != ast::builtins::sampler1d_type
                && ty != ast::builtins::sampler2d_type
                && ty != ast::builtins::sampler3d_type
                && ty != ast::builtins::samplercube_type){
                    auto e = std::make_unique<TypeError>(std::string("Resource `") + _decl->name + "` is not a valid type. (" + _decl->typeExpr->name + ")");
                    e->loc = _decl->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return false;
                }

                /// `buffer<T>` requires exactly one element type. Without this
                /// check a `buffer` with no type args (or an empty `buffer<>`)
                /// reached the INDEX_EXPR path and dereferenced `args[0]` out
                /// of bounds — a hard crash. Catch it here with a clear
                /// diagnostic instead. Builtin element types (`buffer<float4>`,
                /// `buffer<uint>`) are valid and need no further restriction.
                if(ty == ast::builtins::buffer_type && _decl->typeExpr->args.size() != 1){
                    auto e = std::make_unique<TypeError>(std::string("Buffer resource `") + _decl->name + "` must name exactly one element type, e.g. `buffer<float4>`.");
                    e->loc = _decl->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return false;
                }

                /// §2.4 — `uniform<T>` (constant buffer) requires exactly one
                /// type arg, and T must be a user-defined struct. HLSL's
                /// `ConstantBuffer<T>` rejects non-UDT element types, and real
                /// per-frame/per-view constants are always structs — so we
                /// hold all three backends to the portable subset rather than
                /// emitting backend-asymmetric scalar fallbacks.
                if(ty == ast::builtins::uniform_type){
                    if(_decl->typeExpr->args.size() != 1){
                        auto e = std::make_unique<TypeError>(std::string("Uniform resource `") + _decl->name + "` must name exactly one element type, e.g. `uniform<PerFrame>`.");
                        e->loc = _decl->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return false;
                    }
                    auto elemTy = resolveTypeWithExpr(_decl->typeExpr->args[0]);
                    if(elemTy == nullptr){
                        return false;
                    }
                    if(elemTy->builtin){
                        auto e = std::make_unique<TypeError>(std::string("Uniform resource `") + _decl->name + "` must wrap a user-defined struct, got builtin type `" + elemTy->name + "`. Wrap the value in a struct.");
                        e->loc = _decl->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return false;
                    }
                }

                /// 3. (Applies to sampler types) Check sampler state if declared static.

                if(_decl->isStatic && ty != ast::builtins::sampler1d_type
                   && ty != ast::builtins::sampler2d_type
                   && ty != ast::builtins::sampler3d_type
                   && ty != ast::builtins::samplercube_type){
                    auto e = std::make_unique<TypeError>(std::string("Resource `") + _decl->name + "` with type `" + ty->name + "` cannot be declared static unless it is a sampler type.");
                    e->loc = _decl->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return false;
                }

                if(_decl->isStatic){

                }

                /// 4. Validate any `(swizzle=...)` clause attached by the
                /// parser. Encoding 0=Identity, 1=R, 2=G, 3=B, 4=A,
                /// 5=Zero, 6=One. The parser writes a 0 byte for any
                /// character it didn't recognize — we surface a diagnostic
                /// here at the offending column. See
                /// `gte/docs/OmegaSL-Swizzle-Subgroup-Plan.md` §A.5.
                if(_decl->swizzleDesc.has_value()){
                    bool isTexture =
                        ty == ast::builtins::texture1d_type
                        || ty == ast::builtins::texture2d_type
                        || ty == ast::builtins::texture3d_type
                        || ty == ast::builtins::texture1d_array_type
                        || ty == ast::builtins::texture2d_array_type
                        || ty == ast::builtins::texturecube_type
                        || ty == ast::builtins::texturecube_array_type
                        || ty == ast::builtins::texture2d_ms_type
                        || ty == ast::builtins::texture2d_ms_array_type;
                    if(!isTexture){
                        auto e = std::make_unique<TypeError>(
                            std::string("`swizzle` is only valid on texture1d/2d/3d resources, got `")
                            + _decl->typeExpr->name + "`");
                        e->loc = _decl->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return false;
                    }
                    auto & sw = *_decl->swizzleDesc;
                    // 0 means "the parser saw a character it could not
                    // map" — either a non-{r,g,b,a,0,1} char or a missing
                    // position (length < 4).
                    if(sw.r == 0 || sw.g == 0 || sw.b == 0 || sw.a == 0){
                        auto e = std::make_unique<TypeError>(
                            std::string("`swizzle` value must be exactly 4 characters drawn from {r,g,b,a,0,1} (case-insensitive); got `")
                            + std::string(_decl->name) + "`'s clause");
                        e->loc = _decl->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return false;
                    }
                    // Identity (R,G,B,A) → encoding 1,2,3,4 → normalize
                    // to nullopt so the codegen path has a single
                    // "is swizzle present?" check.
                    if(sw.r == 1 && sw.g == 2 && sw.b == 3 && sw.a == 4){
                        _decl->swizzleDesc.reset();
                    }
                }


                break;
            }
            case FUNC_DECL : {
                auto *_decl = (ast::FuncDecl *)decl;
                /// 0. Reserved-name check (§5.1.0 follow-up). A user function
                ///    may not reuse the spelling of a builtin intrinsic — a
                ///    name like `saturate` or `sin` always resolves to the
                ///    builtin, so a same-name user definition was silently
                ///    dead before and is now a hard error. Checked before the
                ///    overload-matching below so the diagnostic is about the
                ///    name itself, not a phantom redeclaration. Fires for
                ///    forward declarations too.
                if(ast::isReservedBuiltinName(_decl->name)){
                    auto e = std::make_unique<ReservedName>(std::string(_decl->name));
                    e->loc = _decl->loc.value_or(ErrorLoc{});
                    diagnostics->addError(std::move(e));
                    return false;
                }
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
                    /// §3.6 — `const` cannot combine with `out` / `inout`.
                    /// Those qualifiers write the caller's storage back
                    /// through the binding; an immutable binding can't be
                    /// written. `const` is only meaningful on an `in` param.
                    if(p.isConst && p.access != ast::AttributedFieldDecl::In){
                        auto e = std::make_unique<TypeError>(
                            std::string("`const` cannot be combined with `out`/`inout` on parameter `") + p.name + "`.");
                        e->loc = _decl->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return false;
                    }
                    currentContext->variableMap.insert(std::make_pair(p.name,
                        SemContext::VarBinding{ p.typeExpr, p.isConst }));
                }

                /// 3. §3.5 — overload-aware prior-decl matching. With
                ///    overloading enabled, "is there a function with
                ///    this name?" is the wrong question — two functions
                ///    can share a name when their parameter lists
                ///    differ. The right question is "is there a function
                ///    with this exact signature?" — if yes, this decl is
                ///    a forward-decl-then-body or a duplicate; if no,
                ///    this decl is a fresh overload regardless of how
                ///    many other same-name functions exist.
                ast::FuncType *existing = nullptr;
                {
                    auto candidates = resolveFuncCandidatesByName(_decl->name);
                    for(auto *cand : candidates){
                        if(cand->builtin) continue;
                        if(cand->paramTypes.size() != _decl->params.size()) continue;
                        bool match = true;
                        for(size_t i = 0; i < _decl->params.size(); ++i){
                            /// Positional comparison via the ordered
                            /// `paramTypes` vector. Avoids the
                            /// unordered_map iteration-order trap that
                            /// the pre-overload single-fn path got
                            /// away with by accident.
                            if(!cand->paramTypes[i]->compare(_decl->params[i].typeExpr)){
                                match = false;
                                break;
                            }
                        }
                        if(match){
                            existing = cand;
                            break;
                        }
                    }
                }
                if(existing != nullptr){
                    /// Same signature — return type must also match,
                    /// otherwise this is a redeclaration with a
                    /// conflicting return type rather than a new
                    /// overload (overload sets cannot differ only by
                    /// return type).
                    if(!existing->returnType->compare(_decl->returnType)){
                        auto e = std::make_unique<TypeError>(std::string("Function `") + _decl->name + "` redeclared with a different return type than the prior declaration.");
                        e->loc = _decl->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return false;
                    }
                    /// Duplicate full definition (forward-decl + body
                    /// is fine; body + body is not).
                    if(!_decl->isForwardDecl && existing->hasDefinition){
                        auto e = std::make_unique<DuplicateDeclaration>(_decl->name);
                        e->loc = _decl->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return false;
                    }
                }

                /// 4. Validate body (skipped for forward declarations).
                if(!_decl->isForwardDecl){
                    auto eval_result = performSemForBlock(*_decl->block, _decl);
                    if(eval_result == nullptr){
                        return false;
                    }
                    if(existing != nullptr){
                        existing->hasDefinition = true;
                    }
                }

                /// 5. Build FuncType and register — only when no prior
                ///    declaration with this exact signature exists.
                ///    A new signature for an existing name is a fresh
                ///    overload and gets its own FuncType in the map.
                if(existing == nullptr){
                    auto *ft = new ast::FuncType();
                    ft->name = _decl->name;
                    ft->declaredScope = ast::builtins::global_scope;
                    ft->builtin = false;
                    ft->returnType = _decl->returnType;
                    ft->hasDefinition = !_decl->isForwardDecl;
                    for(auto & p : _decl->params){
                        ft->fields.insert(std::make_pair(p.name, p.typeExpr));
                        /// §3.5 — keep an ordered parallel list. Used
                        /// by overload resolution and codegen mangling
                        /// where MapVec iteration order would be
                        /// undefined.
                        ft->paramTypes.push_back(p.typeExpr);
                    }
                    currentContext->userFuncTypes.push_back(std::unique_ptr<ast::FuncType>(ft));
                    currentContext->functionMap.push_back(ft);
                }
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

                /// §1.7 — a shader whose output struct carries a `CullDistance`
                /// field uses the Metal-gated feature. Flag it so the file is
                /// expected to `#requires(CULL_DISTANCE)` (the FeatureScanner
                /// warns otherwise) and so it stubs on MSL. Checked against the
                /// return type, where clip/cull are produced.
                for(OmegaCommon::StrRef s : currentContext->cullDistanceStructs){
                    if(s == _decl->returnType->name){
                        _decl->usedFeatures |= OMEGASL_FEATURE_BIT_CULL_DISTANCE;
                        break;
                    }
                }

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
                            if((_t == ast::builtins::sampler1d_type
                                || _t == ast::builtins::sampler2d_type
                                || _t == ast::builtins::sampler3d_type
                                || _t == ast::builtins::samplercube_type) && r.access != ast::ShaderDecl::ResourceMapDesc::In){
                                auto e = std::make_unique<TypeError>(std::string("In Shader Decl `") + _decl->name + "`, resource `" + r.name + "` with type `" + _t->name + "` can only be granted input access to shader.");
                                e->loc = _decl->loc.value_or(ErrorLoc{});
                                diagnostics->addError(std::move(e));
                                return false;
                            }
                            /// §2.4 — constant buffers are read-only on every
                            /// backend; reject `out` / `inout` access.
                            if(_t == ast::builtins::uniform_type && r.access != ast::ShaderDecl::ResourceMapDesc::In){
                                auto e = std::make_unique<TypeError>(std::string("In Shader Decl `") + _decl->name + "`, uniform `" + r.name + "` is read-only and can only be granted input access.");
                                e->loc = _decl->loc.value_or(ErrorLoc{});
                                diagnostics->addError(std::move(e));
                                return false;
                            }
                            /// §2.4 — a uniform's name binds directly to its
                            /// element type T (not the handle type), so member
                            /// access `u.field` resolves through the normal
                            /// struct path and indexing `u[i]` fails with the
                            /// existing "indexing only on buffer/vector/matrix"
                            /// diagnostic. `buffer` keeps binding the handle and
                            /// only resolves T on indexing.
                            if(_t == ast::builtins::uniform_type && !res->typeExpr->args.empty()){
                                currentContext->variableMap.insert(std::make_pair(r.name,
                                    SemContext::VarBinding{ res->typeExpr->args[0], false }));
                            } else {
                                currentContext->variableMap.insert(std::make_pair(r.name,
                                    SemContext::VarBinding{ res->typeExpr, false }));
                            }
                            /// Register element struct type for emission in codegen.
                            if((_t == ast::builtins::buffer_type || _t == ast::builtins::uniform_type)
                               && !res->typeExpr->args.empty()){
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
                        if(p.attributeIndex.has_value()){
                            auto e = std::make_unique<InvalidAttribute>(std::string("Attribute `") + p.attributeName.value() + "` does not take an index in parameter context");
                            e->loc = _decl->loc.value_or(ErrorLoc{});
                            diagnostics->addError(std::move(e));
                            return false;
                        }
                        if(shaderType == ast::ShaderDecl::Fragment){
                            if(p.attributeName.value() == ATTRIBUTE_FRONTFACING && p_type != ast::builtins::bool_type){
                                auto e = std::make_unique<TypeError>(std::string("Attribute `") + ATTRIBUTE_FRONTFACING + "` requires a `bool` parameter, not `" + p_type->name + "`");
                                e->loc = _decl->loc.value_or(ErrorLoc{});
                                diagnostics->addError(std::move(e));
                                return false;
                            }
                            if((p.attributeName.value() == ATTRIBUTE_SAMPLEINDEX
                                || p.attributeName.value() == ATTRIBUTE_INPUT_COVERAGE)
                               && p_type != ast::builtins::uint_type){
                                auto e = std::make_unique<TypeError>(std::string("Attribute `") + p.attributeName.value() + "` requires a `uint` parameter, not `" + p_type->name + "`");
                                e->loc = _decl->loc.value_or(ErrorLoc{});
                                diagnostics->addError(std::move(e));
                                return false;
                            }
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
                    /// §3.6 — same const+out/inout contradiction guard as the
                    /// FuncDecl path. A shader param is realistically only
                    /// ever `in`, but enforce it uniformly.
                    if(p.isConst && p.access != ast::AttributedFieldDecl::In){
                        auto e = std::make_unique<TypeError>(
                            std::string("`const` cannot be combined with `out`/`inout` on parameter `") + p.name + "`.");
                        e->loc = _decl->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        return false;
                    }
                    currentContext->variableMap.insert(std::make_pair(p.name,
                        SemContext::VarBinding{ p.typeExpr, p.isConst }));
                    paramIndex += 1;
                }

                /// 3. Check function block.
                auto eval_result = performSemForBlock(*_decl->block,_decl);

                if(eval_result == nullptr){
                    return false;
                }



                /// 4. Check return types.
                /// (Vertex shaders can return internal struct types. Fragment
                /// shaders return either `float4` (single render target) or an
                /// `internal` struct of attributed fields (`Color(N)`, `Depth`).
                /// Compute shaders do not return any value.)
                if(shaderType == ast::ShaderDecl::Fragment){
                    auto retTy = resolveTypeWithExpr(_decl->returnType);
                    bool isFloat4 = _decl->returnType->compare(ast::TypeExpr::Create(ast::builtins::float4_type));
                    bool isStruct = retTy != nullptr && !retTy->builtin;
                    if(!isFloat4 && !isStruct){
                        auto e = std::make_unique<TypeError>(std::string("Fragment shader `") + _decl->name + "` must return a float4 or an internal struct, not " + _decl->returnType->name);
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
