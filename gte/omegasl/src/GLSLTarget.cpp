#include "Target.h"
#include "AST.h"
#include "CodeGen.h"
#include <ostream>

namespace omegasl {

    GLSLTarget::GLSLTarget() : Target(Target::GLSL) {}
    GLSLTarget::~GLSLTarget() = default;

    OmegaCommon::StrRef GLSLTarget::renameBuiltin(OmegaCommon::StrRef name) {
        if(name == BUILTIN_LERP) return "mix";
        if(name == BUILTIN_FRAC) return "fract";
        if(name == BUILTIN_ATAN2) return "atan";
        return name;
    }

    /// `texelFetch` and `imageStore` take a coord typed by texture
    /// dimensionality: scalar `int` for 1D, `ivec2` for 2D, `ivec3` for 3D.
    /// The previous implementation hardcoded `ivec2`, which silently
    /// truncated 3D coords and miscompiled 1D shaders. Returns nullptr
    /// when the texture type cannot be resolved (caller emits the coord
    /// unmodified, matching pre-fix behavior on that path).
    static const char *glslIntCoordTypeForTexture(CodeGen &cg, ast::Expr *texArg){
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
        if(texTy == builtins::texture1d_type) return "int";
        if(texTy == builtins::texture2d_type) return "ivec2";
        if(texTy == builtins::texture3d_type) return "ivec3";
        return nullptr;
    }

    void GLSLTarget::emitTextureSample(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// GLSL Vulkan: combine separate texture + sampler into combined image-sampler
        /// (e.g. sampler2D(tex, samp)) for texture().
        auto samplerid_expr = (ast::IdExpr *)_expr->args[0];
        auto & sampler_res = *(cg.resourceStore.find(samplerid_expr->id));
        auto t = cg.typeResolver->resolveTypeWithExpr(sampler_res->typeExpr);
        OmegaCommon::String samplerType;
        if(t == ast::builtins::sampler1d_type){
            samplerType = "sampler1D";
        }
        else if(t == ast::builtins::sampler2d_type){
            samplerType = "sampler2D";
        }
        else if(t == ast::builtins::sampler3d_type){
            samplerType = "sampler3D";
        }

        out << "texture(" << samplerType << "(";
        cg.generateExpr(_expr->args[1]);
        out << ",";
        cg.generateExpr(_expr->args[0]);
        out << "),";
        cg.generateExpr(_expr->args[2]);
        out << ")";
    }

    void GLSLTarget::emitTextureRead(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        const char *coordCast = glslIntCoordTypeForTexture(cg, _expr->args[0]);
        out << "texelFetch(";
        cg.generateExpr(_expr->args[0]);
        out << ",";
        if(coordCast){
            out << coordCast << "(";
            cg.generateExpr(_expr->args[1]);
            out << ")";
        } else {
            cg.generateExpr(_expr->args[1]);
        }
        out << ",0)";
    }

    void GLSLTarget::emitTextureWrite(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        const char *coordCast = glslIntCoordTypeForTexture(cg, _expr->args[0]);
        out << "imageStore(";
        cg.generateExpr(_expr->args[0]);
        out << ",";
        if(coordCast){
            out << coordCast << "(";
            cg.generateExpr(_expr->args[1]);
            out << ")";
        } else {
            cg.generateExpr(_expr->args[1]);
        }
        out << ",";
        cg.generateExpr(_expr->args[2]);
        out << ")";
    }

    void GLSLTarget::writeAttribute(OmegaCommon::StrRef attributeName,
                                    std::optional<unsigned> /*attributeIndex*/,
                                    std::ostream &out) {
        /// Map OmegaSL attribute names to their GLSL `gl_*` builtin
        /// equivalents. Indexed `Color(N)` and bare interstage varyings
        /// do not have a single builtin in GLSL — those are routed
        /// through generated `_outColorN` / per-field output variables
        /// by the existing SHADER_DECL/writeInternalFieldRef logic in
        /// GLSLCodeGen and are intentionally not handled here. Callers
        /// that hit this hook for an unsupported attribute get an empty
        /// stream, matching the behavior of the inline lookups that
        /// preceded this hook.
        if(attributeName == ATTRIBUTE_POSITION){
            out << "gl_Position";
        }
        else if(attributeName == ATTRIBUTE_DEPTH){
            out << "gl_FragDepth";
        }
        else if(attributeName == ATTRIBUTE_COVERAGE){
            /// gl_SampleMask is an int[]; index 0 covers up to 32-sample MSAA.
            /// Output form; the input form (gl_SampleMaskIn[0]) is selected
            /// by the SHADER_DECL fragment-input lowering, which doesn't
            /// route through this hook.
            out << "gl_SampleMask[0]";
        }
        else if(attributeName == ATTRIBUTE_VERTEX_ID){
            out << "gl_VertexIndex";
        }
        else if(attributeName == ATTRIBUTE_FRONTFACING){
            out << "gl_FrontFacing";
        }
        else if(attributeName == ATTRIBUTE_SAMPLEINDEX){
            out << "gl_SampleID";
        }
        else if(attributeName == ATTRIBUTE_GLOBALTHREAD_ID){
            out << "gl_GlobalInvocationID";
        }
        else if(attributeName == ATTRIBUTE_LOCALTHREAD_ID){
            out << "gl_LocalInvocationID";
        }
        else if(attributeName == ATTRIBUTE_THREADGROUP_ID){
            out << "gl_WorkGroupID";
        }
    }

    void GLSLTarget::writeTypeName(ast::Type *t, bool pointer, std::ostream &out) {
        if(t == ast::builtins::void_type){
            out << "void";
        }
        else if(t == ast::builtins::bool_type){
            out << "bool";
        }
        else if(t == ast::builtins::float_type){
            out << "float";
        }
        else if(t == ast::builtins::float2_type){
            out << "vec2";
        }
        else if(t == ast::builtins::float3_type){
            out << "vec3";
        }
        else if(t == ast::builtins::float4_type){
            out << "vec4";
        }
        else if(t == ast::builtins::float2x2_type){
            out << "mat2";
        }
        else if(t == ast::builtins::float3x3_type){
            out << "mat3";
        }
        else if(t == ast::builtins::float4x4_type){
            out << "mat4";
        }
        else if(t == ast::builtins::float2x3_type){ out << "mat2x3"; }
        else if(t == ast::builtins::float2x4_type){ out << "mat2x4"; }
        else if(t == ast::builtins::float3x2_type){ out << "mat3x2"; }
        else if(t == ast::builtins::float3x4_type){ out << "mat3x4"; }
        else if(t == ast::builtins::float4x2_type){ out << "mat4x2"; }
        else if(t == ast::builtins::float4x3_type){ out << "mat4x3"; }
        else if(t == ast::builtins::int_type){
            out << "int";
        }
        else if(t == ast::builtins::int2_type){
            out << "ivec2";
        }
        else if(t == ast::builtins::int3_type){
            out << "ivec3";
        }
        else if(t == ast::builtins::int4_type){
            out << "ivec4";
        }
        else if(t == ast::builtins::uint_type){
            out << "uint";
        }
        else if(t == ast::builtins::uint2_type){
            out << "uvec2";
        }
        else if(t == ast::builtins::uint3_type){
            out << "uvec3";
        }
        else if(t == ast::builtins::uint4_type){
            out << "uvec4";
        }
        else {
            out << t->name;
        }

        if(pointer){
            out << " * ";
        }
    }

}
