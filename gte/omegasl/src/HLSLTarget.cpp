#include "Target.h"
#include "AST.h"
#include "CodeGen.h"
#include <ostream>

namespace omegasl {

    HLSLTarget::HLSLTarget() : Target(Target::HLSL) {}
    HLSLTarget::~HLSLTarget() = default;

    OmegaCommon::StrRef HLSLTarget::renameBuiltin(OmegaCommon::StrRef name) {
        /// HLSL's stdlib already uses `lerp` / `frac` / `atan2` natively;
        /// no remap is needed.
        return name;
    }

    /// HLSL `RWTexture<N>D<T>::operator[]` indexes by `uint`/`uint2`/`uint3`.
    /// OmegaSL allows signed coord arithmetic (e.g. `int2(x, y)`), and HLSL
    /// 5.1 accepts the implicit conversion, but stricter SM 6.x DXC settings
    /// warn. Wrap the coord in the matching unsigned type at emit time.
    /// `uint2(uint2_v)` is a no-op, so casting unconditionally based on the
    /// texture type is safe. Returns nullptr when the texture type cannot
    /// be resolved (caller emits the coord unmodified).
    static const char *hlslUintCoordTypeForTexture(CodeGen &cg, ast::Expr *texArg){
        using namespace ast;
        TypeExpr *texTypeExpr = texArg->resolvedType;
        if(!texTypeExpr && texArg->type == ID_EXPR){
            auto *resourceId = static_cast<IdExpr *>(texArg);
            auto it = cg.resourceStore.find(resourceId->id);
            if(it != cg.resourceStore.end()){
                texTypeExpr = (*it)->typeExpr;
            }
        }
        if(!texTypeExpr) return nullptr;
        auto *texTy = cg.typeResolver->resolveTypeWithExpr(texTypeExpr);
        if(texTy == builtins::texture1d_type) return "uint";
        if(texTy == builtins::texture2d_type) return "uint2";
        if(texTy == builtins::texture3d_type) return "uint3";
        return nullptr;
    }

    void HLSLTarget::emitTextureSample(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// Texture has instance method
        cg.generateExpr(_expr->args[1]);
        out << ".Sample(";
        cg.generateExpr(_expr->args[0]);
        out << ",";
        cg.generateExpr(_expr->args[2]);
        out << ")";
    }

    void HLSLTarget::emitTextureRead(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        ast::TypeExpr *textureTypeExpr = _expr->args[0]->resolvedType;
        if (textureTypeExpr == nullptr && _expr->args[0]->type == ID_EXPR) {
            auto *resourceId = static_cast<ast::IdExpr *>(_expr->args[0]);
            auto resourceIt = cg.resourceStore.find(resourceId->id);
            if (resourceIt != cg.resourceStore.end()) {
                textureTypeExpr = (*resourceIt)->typeExpr;
            }
        }
        auto *textureTy =
            textureTypeExpr != nullptr ? cg.typeResolver->resolveTypeWithExpr(textureTypeExpr) : nullptr;
        cg.generateExpr(_expr->args[0]);
        out << ".Load(";
        if (textureTy == ast::builtins::texture1d_type) {
            out << "int2(";
            cg.generateExpr(_expr->args[1]);
            out << ",0)";
        } else if (textureTy == ast::builtins::texture2d_type) {
            out << "int3(";
            cg.generateExpr(_expr->args[1]);
            out << ",0)";
        } else if (textureTy == ast::builtins::texture3d_type) {
            out << "int4(";
            cg.generateExpr(_expr->args[1]);
            out << ",0)";
        } else {
            cg.generateExpr(_expr->args[1]);
        }
        out << ")";
    }

    void HLSLTarget::emitTextureWrite(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// RWTexture<N>D write: texture[coord] = data, with coord cast to
        /// the matching unsigned type (see hlslUintCoordTypeForTexture).
        const char *coordCast = hlslUintCoordTypeForTexture(cg, _expr->args[0]);
        cg.generateExpr(_expr->args[0]);
        out << "[";
        if(coordCast){
            out << coordCast << "(";
            cg.generateExpr(_expr->args[1]);
            out << ")";
        } else {
            cg.generateExpr(_expr->args[1]);
        }
        out << "] = ";
        cg.generateExpr(_expr->args[2]);
    }

    void HLSLTarget::writeAttribute(OmegaCommon::StrRef attributeName,
                                    std::optional<unsigned> attributeIndex,
                                    std::ostream &out) {
        if (attributeName == ATTRIBUTE_VERTEX_ID) {
            out << "SV_VertexID";
        } else if (attributeName == ATTRIBUTE_POSITION) {
            out << "SV_Position";
        } else if (attributeName == ATTRIBUTE_COLOR) {
            /// Indexed `Color(N)` is a fragment-output semantic and maps to
            /// `SV_TargetN`. Bare `Color` keeps its existing user-semantic
            /// meaning for vertex→fragment varyings.
            if (attributeIndex.has_value()) {
                out << "SV_Target" << attributeIndex.value();
            } else {
                out << "COLOR";
            }
        } else if (attributeName == ATTRIBUTE_TEXCOORD) {
            out << "TEXCOORD";
        } else if (attributeName == ATTRIBUTE_DEPTH) {
            out << "SV_Depth";
        } else if (attributeName == ATTRIBUTE_FRONTFACING) {
            out << "SV_IsFrontFace";
        } else if (attributeName == ATTRIBUTE_SAMPLEINDEX) {
            out << "SV_SampleIndex";
        } else if (attributeName == ATTRIBUTE_COVERAGE) {
            out << "SV_Coverage";
        } else if (attributeName == ATTRIBUTE_GLOBALTHREAD_ID) {
            out << "SV_DispatchThreadID";
        } else if (attributeName == ATTRIBUTE_LOCALTHREAD_ID) {
            out << "SV_GroupThreadID";
        } else if (attributeName == ATTRIBUTE_THREADGROUP_ID) {
            out << "SV_GroupID";
        }
    }

    void HLSLTarget::writeTypeName(ast::Type *_ty, bool pointer, std::ostream &out) {
        if (_ty == ast::builtins::bool_type) {
            out << "bool";
        } else if (_ty == ast::builtins::float_type) {
            out << "float";
        } else if (_ty == ast::builtins::float2_type) {
            out << "float2";
        } else if (_ty == ast::builtins::float3_type) {
            out << "float3";
        } else if (_ty == ast::builtins::float4_type) {
            out << "float4";
        } else if (_ty == ast::builtins::float2x2_type) {
            out << "float2x2";
        } else if (_ty == ast::builtins::float3x3_type) {
            out << "float3x3";
        } else if (_ty == ast::builtins::float4x4_type) {
            out << "float4x4";
        } else if (_ty == ast::builtins::float2x3_type) {
            out << "float2x3";
        } else if (_ty == ast::builtins::float2x4_type) {
            out << "float2x4";
        } else if (_ty == ast::builtins::float3x2_type) {
            out << "float3x2";
        } else if (_ty == ast::builtins::float3x4_type) {
            out << "float3x4";
        } else if (_ty == ast::builtins::float4x2_type) {
            out << "float4x2";
        } else if (_ty == ast::builtins::float4x3_type) {
            out << "float4x3";
        } else if (_ty == ast::builtins::int_type) {
            out << "int";
        } else if (_ty == ast::builtins::int2_type) {
            out << "int2";
        } else if (_ty == ast::builtins::int3_type) {
            out << "int3";
        } else if (_ty == ast::builtins::int4_type) {
            out << "int4";
        } else if (_ty == ast::builtins::uint_type) {
            out << "uint";
        } else if (_ty == ast::builtins::uint2_type) {
            out << "uint2";
        } else if (_ty == ast::builtins::uint3_type) {
            out << "uint3";
        } else if (_ty == ast::builtins::uint4_type) {
            out << "uint4";
        } else {
            out << _ty->name;
        }

        if (pointer) {
            out << "*";
        }
    }

}
