#include "AST.def"

#include <optional>

#include <omega-common/utils.h>

#include "omegasl.h"
#include "Error.h"

#ifndef OMEGASL_AST_H
#define OMEGASL_AST_H

namespace omegasl {

    namespace ast {
        /// @brief Defines a Scope in the AST.
        struct Scope {
            OmegaCommon::String name;
            Scope *parent;
            /** @brief Recursively checks to see if this scope is a child of the provided Scope ```scope```.
             * @param scope the scope
             * @returns bool
             * */
            bool isParentScope(Scope *scope) const;
            static Scope *Create(OmegaCommon::StrRef name, Scope *parent);
        };

        struct TypeExpr;

        struct Type {
            OmegaCommon::String name;
            Scope *declaredScope;
            bool builtin = true;
            OmegaCommon::Vector<OmegaCommon::String> typeArgs;
            OmegaCommon::MapVec<OmegaCommon::String,TypeExpr *> fields = {};
        };

        struct FuncType : public Type {
            TypeExpr *returnType;
            /// §3.5 — set once Sema has bound a body to this signature.
            /// With overloading, `name` alone no longer identifies a
            /// definition — two FuncTypes can share a name but differ
            /// by parameter list. The pre-overload `definedFuncNames`
            /// list became unsound (false-positive duplicates) the
            /// moment a second overload was added; the per-FuncType
            /// flag replaces it. Forward declarations leave this
            /// false; the matching body sets it true.
            bool hasDefinition = false;
            /// §3.5 — parameter type list in declaration order. The
            /// inherited `fields` map (a MapVec, which is an
            /// unordered_map) cannot be used for order-sensitive work
            /// like overload mangling because the iteration order is
            /// implementation-defined. This vector is the canonical
            /// positional list. Populated alongside `fields` by Sema's
            /// FUNC_DECL handler.
            OmegaCommon::Vector<TypeExpr *> paramTypes;
        };

        namespace builtins {
            void Initialize();
            void Cleanup();
            extern Scope *global_scope;
#define DECLARE_BUILTIN_TYPE(name) extern Type *name

            DECLARE_BUILTIN_TYPE(void_type);
            DECLARE_BUILTIN_TYPE(bool_type);
            /// §5.2 bool vectors — produced by component-wise compare,
            /// consumed by any/all. Native everywhere; no feature gate.
            DECLARE_BUILTIN_TYPE(bool2_type);
            DECLARE_BUILTIN_TYPE(bool3_type);
            DECLARE_BUILTIN_TYPE(bool4_type);
            DECLARE_BUILTIN_TYPE(int_type);
            DECLARE_BUILTIN_TYPE(int2_type);
            DECLARE_BUILTIN_TYPE(int3_type);
            DECLARE_BUILTIN_TYPE(int4_type);
            DECLARE_BUILTIN_TYPE(uint_type);
            DECLARE_BUILTIN_TYPE(uint2_type);
            DECLARE_BUILTIN_TYPE(uint3_type);
            DECLARE_BUILTIN_TYPE(uint4_type);
            /// §4.1 16-bit float (`half`) — gated on FLOAT16.
            DECLARE_BUILTIN_TYPE(half_type);
            DECLARE_BUILTIN_TYPE(half2_type);
            DECLARE_BUILTIN_TYPE(half3_type);
            DECLARE_BUILTIN_TYPE(half4_type);
            /// §4.1 16-bit signed/unsigned ints (`short`/`ushort`) —
            /// share the FLOAT16 gate per Feature-Gap-Survey rationale
            /// (every backend that ships float16 also ships int16).
            DECLARE_BUILTIN_TYPE(short_type);
            DECLARE_BUILTIN_TYPE(short2_type);
            DECLARE_BUILTIN_TYPE(short3_type);
            DECLARE_BUILTIN_TYPE(short4_type);
            DECLARE_BUILTIN_TYPE(ushort_type);
            DECLARE_BUILTIN_TYPE(ushort2_type);
            DECLARE_BUILTIN_TYPE(ushort3_type);
            DECLARE_BUILTIN_TYPE(ushort4_type);
            /// §4.2 64-bit ints (`long`/`ulong`) — gated on INT64.
            DECLARE_BUILTIN_TYPE(long_type);
            DECLARE_BUILTIN_TYPE(long2_type);
            DECLARE_BUILTIN_TYPE(long3_type);
            DECLARE_BUILTIN_TYPE(long4_type);
            DECLARE_BUILTIN_TYPE(ulong_type);
            DECLARE_BUILTIN_TYPE(ulong2_type);
            DECLARE_BUILTIN_TYPE(ulong3_type);
            DECLARE_BUILTIN_TYPE(ulong4_type);
            DECLARE_BUILTIN_TYPE(float_type);
            DECLARE_BUILTIN_TYPE(float2_type);
            DECLARE_BUILTIN_TYPE(float3_type);
            DECLARE_BUILTIN_TYPE(float4_type);
            DECLARE_BUILTIN_TYPE(float2x2_type);
            DECLARE_BUILTIN_TYPE(float3x3_type);
            DECLARE_BUILTIN_TYPE(float4x4_type);
            DECLARE_BUILTIN_TYPE(float2x3_type);
            DECLARE_BUILTIN_TYPE(float2x4_type);
            DECLARE_BUILTIN_TYPE(float3x2_type);
            DECLARE_BUILTIN_TYPE(float3x4_type);
            DECLARE_BUILTIN_TYPE(float4x2_type);
            DECLARE_BUILTIN_TYPE(float4x3_type);
            /// §12.2 follow-up — integer matrix types. Lowered to an array of
            /// C integer column-vectors (`int4 m[C]` / `ivec4 m[C]`) on every
            /// backend; no native integer matrix exists on GLSL/MSL. Support
            /// declaration / indexing / buffer round-trip only — matrix
            /// algebra and inline construction are rejected in Sema.
            DECLARE_BUILTIN_TYPE(int2x2_type);
            DECLARE_BUILTIN_TYPE(int3x3_type);
            DECLARE_BUILTIN_TYPE(int4x4_type);
            DECLARE_BUILTIN_TYPE(int2x3_type);
            DECLARE_BUILTIN_TYPE(int2x4_type);
            DECLARE_BUILTIN_TYPE(int3x2_type);
            DECLARE_BUILTIN_TYPE(int3x4_type);
            DECLARE_BUILTIN_TYPE(int4x2_type);
            DECLARE_BUILTIN_TYPE(int4x3_type);
            DECLARE_BUILTIN_TYPE(uint2x2_type);
            DECLARE_BUILTIN_TYPE(uint3x3_type);
            DECLARE_BUILTIN_TYPE(uint4x4_type);
            DECLARE_BUILTIN_TYPE(uint2x3_type);
            DECLARE_BUILTIN_TYPE(uint2x4_type);
            DECLARE_BUILTIN_TYPE(uint3x2_type);
            DECLARE_BUILTIN_TYPE(uint3x4_type);
            DECLARE_BUILTIN_TYPE(uint4x2_type);
            DECLARE_BUILTIN_TYPE(uint4x3_type);

            DECLARE_BUILTIN_TYPE(buffer_type);
            /// §2.4 constant / uniform buffer (read-only, value-access).
            DECLARE_BUILTIN_TYPE(uniform_type);
            DECLARE_BUILTIN_TYPE(texture1d_type);
            DECLARE_BUILTIN_TYPE(texture2d_type);
            DECLARE_BUILTIN_TYPE(texture3d_type);
            DECLARE_BUILTIN_TYPE(texture1d_array_type);
            DECLARE_BUILTIN_TYPE(texture2d_array_type);
            DECLARE_BUILTIN_TYPE(texturecube_type);
            DECLARE_BUILTIN_TYPE(texturecube_array_type);
            DECLARE_BUILTIN_TYPE(texture2d_ms_type);
            DECLARE_BUILTIN_TYPE(texture2d_ms_array_type);

            DECLARE_BUILTIN_TYPE(sampler1d_type);
            DECLARE_BUILTIN_TYPE(sampler2d_type);
            DECLARE_BUILTIN_TYPE(sampler3d_type);
            DECLARE_BUILTIN_TYPE(samplercube_type);

#undef  DECLARE_BUILTIN_TYPE
#define DECLARE_BUILTIN_FUNC(name) extern FuncType *name;

            DECLARE_BUILTIN_FUNC(make_float2);
            DECLARE_BUILTIN_FUNC(make_float3);
            DECLARE_BUILTIN_FUNC(make_float4);
            /// §5.2 bool-vector constructors.
            DECLARE_BUILTIN_FUNC(make_bool2);
            DECLARE_BUILTIN_FUNC(make_bool3);
            DECLARE_BUILTIN_FUNC(make_bool4);
            DECLARE_BUILTIN_FUNC(make_int2);
            DECLARE_BUILTIN_FUNC(make_int3);
            DECLARE_BUILTIN_FUNC(make_int4);
            DECLARE_BUILTIN_FUNC(make_uint2);
            DECLARE_BUILTIN_FUNC(make_uint3);
            DECLARE_BUILTIN_FUNC(make_uint4);
            /// §4.1 vector constructors for the 16-bit family.
            DECLARE_BUILTIN_FUNC(make_half2);
            DECLARE_BUILTIN_FUNC(make_half3);
            DECLARE_BUILTIN_FUNC(make_half4);
            DECLARE_BUILTIN_FUNC(make_short2);
            DECLARE_BUILTIN_FUNC(make_short3);
            DECLARE_BUILTIN_FUNC(make_short4);
            DECLARE_BUILTIN_FUNC(make_ushort2);
            DECLARE_BUILTIN_FUNC(make_ushort3);
            DECLARE_BUILTIN_FUNC(make_ushort4);
            /// §4.2 vector constructors for the 64-bit int family.
            DECLARE_BUILTIN_FUNC(make_long2);
            DECLARE_BUILTIN_FUNC(make_long3);
            DECLARE_BUILTIN_FUNC(make_long4);
            DECLARE_BUILTIN_FUNC(make_ulong2);
            DECLARE_BUILTIN_FUNC(make_ulong3);
            DECLARE_BUILTIN_FUNC(make_ulong4);
            DECLARE_BUILTIN_FUNC(make_float2x2);
            DECLARE_BUILTIN_FUNC(make_float3x3);
            DECLARE_BUILTIN_FUNC(make_float4x4);
            DECLARE_BUILTIN_FUNC(make_float2x3);
            DECLARE_BUILTIN_FUNC(make_float2x4);
            DECLARE_BUILTIN_FUNC(make_float3x2);
            DECLARE_BUILTIN_FUNC(make_float3x4);
            DECLARE_BUILTIN_FUNC(make_float4x2);
            DECLARE_BUILTIN_FUNC(make_float4x3);

            DECLARE_BUILTIN_FUNC(dot);

            DECLARE_BUILTIN_FUNC(cross);

            DECLARE_BUILTIN_FUNC(sample);
            DECLARE_BUILTIN_FUNC(sampleLOD);
            DECLARE_BUILTIN_FUNC(sampleBias);
            DECLARE_BUILTIN_FUNC(sampleGrad);
            DECLARE_BUILTIN_FUNC(gather);
            DECLARE_BUILTIN_FUNC(gatherRed);
            DECLARE_BUILTIN_FUNC(gatherGreen);
            DECLARE_BUILTIN_FUNC(gatherBlue);
            DECLARE_BUILTIN_FUNC(gatherAlpha);
            DECLARE_BUILTIN_FUNC(write);
            DECLARE_BUILTIN_FUNC(read);
            DECLARE_BUILTIN_FUNC(calculateLOD);
            DECLARE_BUILTIN_FUNC(getDimensions);
        }

        /// §5.1.0 — map a builtin alias spelling to its canonical name
        /// (`mod`→`fmod`, `mad`→`fma`). Any other name is returned
        /// unchanged. Used by Sema (dispatch) and CodeGen (emission) so
        /// the alias is resolved at every point that recognizes a builtin.
        OmegaCommon::StrRef canonicalBuiltinAlias(OmegaCommon::StrRef name);

        /// §5.1.0 follow-up — true if `name` is the spelling of an OmegaSL
        /// builtin function (a math/geometric intrinsic, a `make_*`
        /// constructor, a texture op, a barrier, or one of the `mod`/`mad`
        /// aliases). Sema rejects a user `func` declaration that reuses one
        /// of these names: a builtin like `sin` always means the builtin,
        /// the way `transpose` or `sample` do. This is the OmegaSL-side
        /// reservation — distinct from the unconditional `osl_user_` prefix
        /// (CodeGen), which defends against *backend* stdlib names OmegaSL
        /// doesn't even model (e.g. Metal `add_const`). Kept in sync with
        /// the builtin dispatch in Sema and the `builtinFunctionMap`.
        bool isReservedBuiltinName(OmegaCommon::StrRef name);

        /// @brief Refers to a type that already exists.
        struct TypeExpr {
            OmegaCommon::String name;
            bool pointer;
            bool hasTypeArgs;
            OmegaCommon::Vector<TypeExpr *> args;
            /// Array dimensions, outermost first. Empty for a non-array
            /// type; `[16][16]` yields `{16, 16}`. Backends emit one
            /// `[d]` suffix per entry in order.
            OmegaCommon::Vector<unsigned> arrayDims;

            static TypeExpr *Create(OmegaCommon::StrRef name, bool pointer = false,bool hasTypeArgs = false,OmegaCommon::Vector<TypeExpr *> * args = nullptr);
            static TypeExpr *Create(Type * type, bool pointer = false);
            bool compare(TypeExpr *other);
            ~TypeExpr();
        };

        struct FuncDecl;
        struct StructDecl;
        struct Expr;

        /// @brief Provides useful semantics info about AST Nodes.
        /// @paragraph This includes resolving ast::Type using ast::TypeExpr
        /// and retrieving StructDecls used in a ShaderDecl
        class SemFrontend {
        public:
            virtual TypeExpr *evalExprForTypeExpr(Expr *expr) = 0;

            virtual void getStructsInFuncDecl(FuncDecl *funcDecl,std::vector<std::string> & out) = 0;
            /** @brief Retrieves the underlying Type associated with this TypeExpr
             * @param expr The TypeExpr to evalutate.
             * @returns Type **/
            virtual Type *resolveTypeWithExpr(TypeExpr *expr) = 0;
        };

        /// @brief A Node in the AST
        struct Stmt {
            ASTType type;
            Scope *scope;
            std::optional<ErrorLoc> loc;
        };

        struct Expr;

        struct Decl : public Stmt {};

        /// @brief Declares a Resource on the GPU.
        /// @paragraph All resources are dynamically uploaded onto the GPU,
        /// but samplers unlike any other resource can be static declared
        /// and preloaded onto the GPU with a set configuration throughout the duration of execution.
        struct ResourceDecl : public Decl {
            TypeExpr *typeExpr;
            OmegaCommon::String name;
            size_t registerNumber;
            bool isStatic = false;
            struct StaticSamplerDesc {
                omegasl_shader_static_sampler_address_mode
                uAddressMode = OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_WRAP,
                vAddressMode = OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_WRAP,
                wAddressMode = OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_WRAP;
                omegasl_shader_static_sampler_filter filter = OMEGASL_SHADER_SAMPLER_LINEAR_FILTER;
                unsigned int maxAnisotropy = 16;
            };
            std::unique_ptr<StaticSamplerDesc> staticSamplerDesc;

            /// Optional channel swizzle parsed from a `(swizzle=<rgba>)`
            /// clause on a texture resource declaration. Encoding matches
            /// `omegasl_texture_swizzle_desc` in omegasl.h:
            /// `0 = Identity, 1 = R, 2 = G, 3 = B, 4 = A, 5 = Zero, 6 = One`.
            /// Sema normalizes the identity swizzle (`rgba`) to `nullopt`
            /// so the codegen path has a single "is swizzle present?"
            /// check. Absent => the runtime treats the layout descriptor's
            /// `swizzle_desc` as identity.
            struct SwizzleDesc {
                unsigned char r = 0;
                unsigned char g = 0;
                unsigned char b = 0;
                unsigned char a = 0;
            };
            std::optional<SwizzleDesc> swizzleDesc;
        };

        struct AttributedFieldDecl {
            TypeExpr *typeExpr;
            OmegaCommon::String name;
            std::optional<OmegaCommon::String> attributeName;
            /// @brief Optional index for indexed attributes such as `Color(N)`.
            /// Present only when the attribute was written with a parenthesised
            /// integer (e.g. `: Color(1)`).
            std::optional<unsigned> attributeIndex;

            /// §3.7 — `in` / `out` / `inout` qualifier on a function parameter.
            /// Only meaningful for `FuncDecl::params`; struct fields and shader
            /// params leave this at `In`. `in` is the default (the param is
            /// read-only from the caller's perspective). `out` writes back the
            /// callee's final value; `inout` does both. Spelling per backend:
            ///   HLSL  → `out T name` / `inout T name`
            ///   MSL   → `thread T& name` (covers both — Metal has no
            ///            write-only reference qualifier)
            ///   GLSL  → `out T name` / `inout T name`
            enum ParamAccess { In, Out, Inout };
            ParamAccess access = In;

            /// §3.6 — `const` qualifier on a function parameter. Set by the
            /// parser for either the prefix (`const T x`) or postfix
            /// (`T const x`) spelling. Only meaningful for `FuncDecl::params`;
            /// struct fields leave it at false. Sema rejects writes through
            /// the binding (reusing the const-local machinery) and rejects
            /// combining it with `out` / `inout`, which would be
            /// contradictory. CodeGen prefixes the emitted parameter with
            /// `const`, which all three backends accept verbatim.
            bool isConst = false;
        };

        struct VarDecl : public Decl {
            TypeExpr *typeExpr;
            /// §3.6 — `const` qualifier on a local declaration. Set by the
            /// parser when `const` precedes the type. Sema rejects writes
            /// through this name; CodeGen prefixes the emitted declaration
            /// with `const`, which all three backends accept verbatim.
            bool isConst = false;
            /// §6.1 — `threadgroup` storage qualifier. Set by the parser
            /// when `threadgroup` precedes the type. Sema requires the
            /// enclosing function to be a compute shader. CodeGen emits it
            /// inline on MSL (`threadgroup T name`) and hoists it to file
            /// scope on HLSL (`groupshared`) / GLSL (`shared`).
            bool isThreadgroup = false;
            struct Spec {
                OmegaCommon::String name;
                std::optional<ast::Expr *> initializer;
            } spec;
        };

        struct ReturnDecl : public Decl {
            Expr *expr;
        };

        struct Block {
            OmegaCommon::Vector<ast::Stmt *> body;
        };

        /// @brief else-if branch: condition and block
        struct ElseIfBranch {
            Expr *condition;
            std::unique_ptr<Block> block;
        };

        /// @brief if / else if / else statement
        struct IfStmt : public Stmt {
            Expr *condition;
            std::unique_ptr<Block> thenBlock;
            OmegaCommon::Vector<ElseIfBranch> elseIfs;
            std::unique_ptr<Block> elseBlock;  /// optional
        };

        /// @brief for loop: init; condition; increment { body }
        struct ForStmt : public Stmt {
            Stmt *init;       /// VAR_DECL or expression statement
            Expr *condition;
            Expr *increment;
            std::unique_ptr<Block> body;
        };

        /// @brief while loop
        struct WhileStmt : public Stmt {
            Expr *condition;
            std::unique_ptr<Block> body;
        };

        /// @brief One labeled branch inside a `switch` body. `value` is the
        /// constant case expression for `case <expr>:`, or `nullptr` for
        /// `default:`. `body` holds every statement between this label and
        /// the next case/default — fall-through to the next case is C-style
        /// (terminate with `break;` to stop falling through).
        struct SwitchCase {
            Expr *value;
            OmegaCommon::Vector<Stmt *> body;
        };

        /// @brief switch / case / default statement. C-style fall-through
        /// semantics: control flows from the matched case into subsequent
        /// cases unless `break;` appears. `default` is optional and may
        /// appear in any position; only one `default` is allowed per switch.
        struct SwitchStmt : public Stmt {
            Expr *condition;
            OmegaCommon::Vector<SwitchCase> cases;
        };

        /// @brief `break;` statement — exits the innermost enclosing
        /// `for`/`while`/`switch` block.
        struct BreakStmt : public Stmt {};

        /// @brief `continue;` statement — jumps to the next iteration of the
        /// innermost `for`/`while` loop.
        struct ContinueStmt : public Stmt {};

        /// @brief `discard;` statement — kills the current fragment.
        /// Only valid inside a `fragment` shader.
        struct DiscardStmt : public Stmt {};

        /// @brief Declares a Struct.
        /// @paragraph Can be either public or for shader internal use only
        /// by marking it with the `internal` keyword)
        struct StructDecl : public Decl {
            OmegaCommon::String name;
            bool internal;
            OmegaCommon::Vector<AttributedFieldDecl> fields;
        };

        /// @brief Declares a Function
        struct FuncDecl : public Decl {
            OmegaCommon::String name;
            OmegaCommon::Vector<AttributedFieldDecl> params;
            TypeExpr *returnType;
            std::unique_ptr<ast::Block> block;
            /// @brief True when this is a forward declaration (prototype only,
            /// terminated by `;` with no body). The body is supplied by a
            /// later full definition with a matching signature.
            bool isForwardDecl = false;
            /// @brief Bitfield of OMEGASL_FEATURE_BIT_* values inferred from
            /// the function body by the post-parse portability scanner
            /// (Layer 2, Feature-Gap-Survey §14.2). For shaders, this is
            /// the *transitive* set — own body uses unioned with every
            /// user function transitively called. Independent of the
            /// file-scope `#requires(...)` set: this is "what the body
            /// actually uses," not "what the author declared." The
            /// scanner compares the two to emit advisory warnings
            /// (undeclared-use + partition-suggestion); compilation is
            /// not gated on the result.
            uint64_t usedFeatures = 0;
        };

        /// @brief Declares a Shader.
        struct ShaderDecl : public FuncDecl {
            typedef enum : int {
                Vertex,
                Fragment,
                Compute,
                Hull,
                Domain
            } Type;
            Type shaderType;
            struct ResourceMapDesc {
                typedef enum : int {
                    In,
                    Out,
                    Inout
                } Access;
                Access access;
                OmegaCommon::String name;
            };
            OmegaCommon::Vector<ResourceMapDesc> resourceMap;
            struct {
                unsigned x,y,z;
            } threadgroupDesc;
            struct TessellationDesc {
                typedef enum : int {
                    Triangle,
                    Quad
                } Domain;
                typedef enum : int {
                    Integer,
                    FractionalEven,
                    FractionalOdd
                } Partitioning;
                typedef enum : int {
                    TriangleCW,
                    TriangleCCW,
                    Line
                } OutputTopology;
                Domain domain = Triangle;
                Partitioning partitioning = Integer;
                OutputTopology outputTopology = TriangleCW;
                unsigned outputControlPoints = 3;
            };
            TessellationDesc tessDesc;
        };

        struct Expr : public Stmt {
            TypeExpr *resolvedType = nullptr;
        };

        struct IdExpr : public Expr {
            OmegaCommon::String id;
        };

        struct LiteralExpr : public Expr {

            /// Num Literal
            std::optional<float> f_num;
            std::optional<int> i_num;
            std::optional<unsigned int> ui_num;
            std::optional<double> d_num;
            /// §4.2 64-bit integer literals (`123L` / `123UL`). Storing
            /// the wide value preserves bit patterns that wouldn't fit
            /// in i_num/ui_num. Half literals (`1.0h`) reuse f_num — the
            /// 16-bit precision contract is enforced at type resolution
            /// time, not at the literal level.
            std::optional<int64_t> i64_num;
            std::optional<uint64_t> ui64_num;
            /// Bool Literal
            std::optional<bool> b_val;
            /// Str Literal
            std::optional<OmegaCommon::String> str;

            bool isFloat() const;
            bool isInt() const;
            bool isUint() const;
            bool isDouble() const;
            bool isLong() const;
            bool isUlong() const;
            bool isBool() const;

            bool isString() const;
        };

        struct MemberExpr : public Expr {
            Expr *lhs;
            OmegaCommon::String rhs_id;
        };

        struct ArrayExpr : public Expr {
            OmegaCommon::Vector<Expr *> elm;
        };

        struct CallExpr : public Expr {
            Expr *callee;
            OmegaCommon::Vector<Expr *> args;
            /// §3.5 — set by Sema once the callee is resolved against
            /// the user-function overload set. Codegen reads this to
            /// pick the matching mangled spelling at the call site
            /// (the ID_EXPR alone is ambiguous when more than one
            /// overload shares the name). Stays null for builtin
            /// calls and for unresolved/error cases.
            FuncType *resolvedCallee = nullptr;
        };

        struct UnaryOpExpr : public Expr {
            bool isPrefix;
            OmegaCommon::String op;
            Expr *expr;
        };

        struct BinaryExpr : public Expr {
            OmegaCommon::String op;
            Expr *lhs;
            Expr *rhs;
        };

        struct PointerExpr : public Expr {
            typedef enum : int {
                AddressOf,
                Dereference
            } Type;
            Type ptType;
            Expr *expr;
        };

        struct IndexExpr : public Expr {
            Expr *lhs;
            Expr *idx_expr;
        };

        struct CastExpr : public Expr {
            TypeExpr *targetType;
            Expr *expr;
        };

        /// §3.2 — `cond ? thenExpr : elseExpr`. Sema resolves the
        /// expression's type to the common type of `thenExpr` and
        /// `elseExpr`; codegen emits `(cond ? a : b)` straight through
        /// (the spelling is identical on every backend).
        struct TernaryExpr : public Expr {
            Expr *condition;
            Expr *thenExpr;
            Expr *elseExpr;
        };

    }
}
#endif