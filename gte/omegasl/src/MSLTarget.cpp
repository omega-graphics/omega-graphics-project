#include "Target.h"
#include "AST.h"
#include "CodeGen.h"
#include <ostream>

namespace omegasl {

    MSLTarget::MSLTarget() : Target(Target::MSL) {}
    MSLTarget::~MSLTarget() = default;

    OmegaCommon::StrRef MSLTarget::renameBuiltin(OmegaCommon::StrRef name) {
        if(name == BUILTIN_LERP) return "mix";
        if(name == BUILTIN_FRAC) return "fract";
        return name;
    }

    /// Metal's `texture<T>::read` / `::write` take unsigned coords
    /// (`uint`/`uint2`/`uint3`). OmegaSL accepts both signed and unsigned
    /// coords at the language level — building a coord via `int2(x, y)`
    /// after signed arithmetic is common — so we cast to the matching
    /// unsigned type at emit time. Without this, `tex.read(int2(...))`
    /// produces "no matching member function" errors against the
    /// ushort2/uint2 overloads. `uint2(uint2_v)` is a no-op, so casting
    /// unconditionally based on the texture type is safe for shaders
    /// that already use unsigned coords. Returns nullptr if the texture
    /// type can't be resolved (caller emits the coord unmodified).
    static const char *metalUintCoordTypeForTexture(CodeGen &cg, ast::Expr *texArg){
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

    void MSLTarget::emitTextureSample(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        cg.generateExpr(_expr->args[1]);
        out << ".sample";
        out << "(";
        cg.generateExpr(_expr->args[0]);
        out << ",";
        cg.generateExpr(_expr->args[2]);
        out << ")";
    }

    void MSLTarget::emitTextureRead(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        const char *coordCast = metalUintCoordTypeForTexture(cg, _expr->args[0]);
        cg.generateExpr(_expr->args[0]);
        out << ".read(";
        if(coordCast){
            out << coordCast << "(";
            cg.generateExpr(_expr->args[1]);
            out << ")";
        } else {
            cg.generateExpr(_expr->args[1]);
        }
        out << ")";
    }

    void MSLTarget::emitTextureWrite(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        const char *coordCast = metalUintCoordTypeForTexture(cg, _expr->args[0]);
        cg.generateExpr(_expr->args[0]);
        out << ".write(";
        cg.generateExpr(_expr->args[2]);
        out << ",";
        if(coordCast){
            out << coordCast << "(";
            cg.generateExpr(_expr->args[1]);
            out << ")";
        } else {
            cg.generateExpr(_expr->args[1]);
        }
        out << ")";
    }

    void MSLTarget::writeAttribute(OmegaCommon::StrRef attributeName,
                                   std::optional<unsigned> attributeIndex,
                                   std::ostream &out) {
        if(attributeName == ATTRIBUTE_POSITION){
            out << "position";
        }
        else if(attributeName == ATTRIBUTE_VERTEX_ID){
            out << "vertex_id";
        }
        else if(attributeName == ATTRIBUTE_INSTANCE_ID){
            out << "instance_id";
        }
        else if(attributeName == ATTRIBUTE_COLOR){
            /// Indexed `Color(N)` is a fragment-output target; bare `Color`
            /// flows through MSL untagged (the field is just a varying).
            if(attributeIndex.has_value()){
                out << "color(" << attributeIndex.value() << ")";
            }
        }
        else if(attributeName == ATTRIBUTE_DEPTH){
            out << "depth(any)";
        }
        else if(attributeName == ATTRIBUTE_FRONTFACING){
            out << "front_facing";
        }
        else if(attributeName == ATTRIBUTE_SAMPLEINDEX){
            out << "sample_id";
        }
        else if(attributeName == ATTRIBUTE_COVERAGE){
            out << "sample_mask";
        }
        else if(attributeName == ATTRIBUTE_GLOBALTHREAD_ID){
            out << "thread_position_in_grid";
        }
        else if(attributeName == ATTRIBUTE_THREADGROUP_ID){
            out << "threadgroup_position_in_grid";
        }
        else if(attributeName == ATTRIBUTE_LOCALTHREAD_ID){
            out << "thread_position_in_threadgroup";
        }
    }

    void MSLTarget::writeTypeName(ast::Type *_t, bool pointer, std::ostream &out) {
        using namespace ast;

        if(_t == builtins::void_type){
            out << "void";
        }
        else if(_t == builtins::bool_type){
            out << "bool";
        }
        else if(_t == builtins::int_type){
            out << "int";
        }
        else if(_t == builtins::int2_type){
            out << "int2";
        }
        else if(_t == builtins::int3_type){
            out << "int3";
        }
        else if(_t == builtins::int4_type){
            out << "int4";
        }
        else if(_t == builtins::uint_type){
            out << "uint";
        }
        else if(_t == builtins::uint2_type){
            out << "uint2";
        }
        else if(_t == builtins::uint3_type){
            out << "uint3";
        }
        else if(_t == builtins::uint4_type){
            out << "uint4";
        }
        else if(_t == builtins::float_type){
            out << "float";
        }
        else if(_t == builtins::float2_type){
            out << "float2";
        }
        else if(_t == builtins::float3_type){
            out << "float3";
        }
        else if(_t == builtins::float4_type){
            out << "float4";
        }
        else if(_t == builtins::float2x2_type){
            out << "float2x2";
        }
        else if(_t == builtins::float3x3_type){
            out << "float3x3";
        }
        else if(_t == builtins::float4x4_type){
            out << "float4x4";
        }
        else if(_t == builtins::float2x3_type){ out << "float2x3"; }
        else if(_t == builtins::float2x4_type){ out << "float2x4"; }
        else if(_t == builtins::float3x2_type){ out << "float3x2"; }
        else if(_t == builtins::float3x4_type){ out << "float3x4"; }
        else if(_t == builtins::float4x2_type){ out << "float4x2"; }
        else if(_t == builtins::float4x3_type){ out << "float4x3"; }
        else if(_t == builtins::sampler1d_type || _t == builtins::sampler2d_type || _t == builtins::sampler3d_type){
            out << "sampler";
        }
        else {
            out << _t->name;
        }


        if(pointer){
            out << " *";
        }
    }

}
