#include "Target.h"
#include "AST.h"
#include "CodeGen.h"
#include <ostream>
#include <sstream>
#include <unordered_set>
#include <string>

#ifdef TARGET_DIRECTX
#    include <d3dcompiler.h>
#    pragma comment(lib, "d3dcompiler.lib")
#endif

#define HLSL_TEXTURE1D    "Texture1D"
#define HLSL_RW_TEXTURE1D "RWTexture1D"
#define HLSL_TEXTURE2D    "Texture2D"
#define HLSL_RW_TEXTURE2D "RWTexture2D"
#define HLSL_BUFFER       "StructuredBuffer"
#define HLSL_RW_BUFFER    "RWStructuredBuffer"
#define HLSL_SAMPLER      "SamplerState"

namespace omegasl {

    HLSLTarget::HLSLTarget(HLSLCodeOpts &opts) : Target(Target::HLSL), opts(opts) {}
    HLSLTarget::~HLSLTarget() = default;

    const char *HLSLTarget::shaderFileExt(ast::ShaderDecl::Type /*stage*/) const {
        return ".hlsl";
    }

    bool HLSLTarget::compileShader(ast::ShaderDecl::Type stage,
                                   OmegaCommon::StrRef name,
                                   const OmegaCommon::FS::Path &srcDir,
                                   const OmegaCommon::FS::Path &outDir) {
        std::ostringstream out;
        out << " -nologo -T";
        if (stage == ast::ShaderDecl::Vertex) {
            out << "vs_5_0";
        } else if (stage == ast::ShaderDecl::Fragment) {
            out << "ps_5_0";
        } else if (stage == ast::ShaderDecl::Compute) {
            out << "cs_5_0";
        } else if (stage == ast::ShaderDecl::Hull) {
            out << "hs_5_0";
        } else if (stage == ast::ShaderDecl::Domain) {
            out << "ds_5_0";
        }
        out << " -E" << name.data() << " -Fo "
            << OmegaCommon::FS::Path(outDir).append(name).concat(".cso").absPath();
        out << " " << OmegaCommon::FS::Path(srcDir).append(name).concat(shaderFileExt(stage)).absPath() << " /Zi";

        auto dxc_process = OmegaCommon::ChildProcess::OpenWithStdoutPipe(opts.dxc_cmd, out.str().c_str());
        auto res = dxc_process.wait();

        if (res != 0) {
            std::cerr << "error: dxc failed (exit " << res << ") for shader '" << name.data() << "'" << std::endl;
            return false;
        }
        return true;
    }

    void HLSLTarget::compileShaderRuntime(ast::ShaderDecl::Type stage,
                                          OmegaCommon::StrRef name,
                                          const std::string &source,
                                          omegasl_shader &meta) {
#ifdef TARGET_DIRECTX
        std::cout << "[OmegaSL HLSL] Compiling shader '" << name.data() << "' target="
                  << (stage == ast::ShaderDecl::Vertex     ? "vs"
                      : stage == ast::ShaderDecl::Fragment ? "ps"
                                                          : "cs")
                  << "\n"
                  << source << std::endl;
        ID3DBlob *blob = nullptr;
        OmegaCommon::String target;
        if (stage == ast::ShaderDecl::Vertex) {
            target = "vs_5_1";
        } else if (stage == ast::ShaderDecl::Fragment) {
            target = "ps_5_1";
        } else if (stage == ast::ShaderDecl::Compute) {
            target = "cs_5_1";
        }

        ID3DBlob *errorBlob = nullptr;

        D3DCompile(source.data(), source.size(), name.data(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                   name.data(), target.data(), D3DCOMPILE_DEBUG, NULL, &blob, &errorBlob);

        if (errorBlob != nullptr) {
            std::cout << "OMEGASL COMPILE ERROR: D3D ERROR:" << (char *)errorBlob->GetBufferPointer() << std::endl;
        }

        if (blob != nullptr) {
            meta.data = blob->GetBufferPointer();
            meta.dataSize = blob->GetBufferSize();
        }
#else
        (void)stage;
        (void)name;
        (void)source;
        (void)meta;
#endif
    }

    /// Curated set of HLSL stdlib identifiers that a user function name
    /// would shadow. Names listed here trigger `osl_user_<name>` mangling
    /// at both definition and call sites. Names not in this set pass
    /// through unchanged so the generated source stays readable. Add
    /// entries when a real collision is observed.
    bool HLSLTarget::needsMangling(OmegaCommon::StrRef name) const {
        static const std::unordered_set<std::string> hlslStdlib = {
            "abs","acos","all","any","asin","atan","atan2","ceil","clamp",
            "clip","cos","cosh","cross","ddx","ddy","degrees","determinant",
            "distance","dot","exp","exp2","floor","fmod","frac","frexp",
            "fwidth","isfinite","isinf","isnan","ldexp","length","lerp","lit",
            "log","log10","log2","mad","max","min","modf","mul","normalize",
            "pow","radians","reflect","refract","round","rsqrt","saturate",
            "sign","sin","sinh","smoothstep","sqrt","step","tan","tanh",
            "transpose","trunc"
        };
        return hlslStdlib.count(std::string(name)) > 0;
    }

    void HLSLTarget::resetForNextShader() {
        tResourceCount = 0;
        uResourceCount = 0;
        sResourceCount = 0;
    }

    void HLSLTarget::emitStaticPreamble(std::ostream &/*out*/) {
        /// HLSL emits static samplers inline as `: register(sN, spaceM);`
        /// during emitResourceBinding — no separate preamble.
    }

    void HLSLTarget::emitResourceBinding(CodeGen &cg,
                                         ast::ResourceDecl *res_desc,
                                         ast::ShaderDecl *shader,
                                         omegasl_shader_layout_desc_io_mode ioMode,
                                         std::ostream &out,
                                         omegasl_shader_layout_desc &layoutDesc) {
        layoutDesc.offset = 0;
        layoutDesc.io_mode = ioMode;

        auto _t = cg.typeResolver->resolveTypeWithExpr(res_desc->typeExpr);

        bool isTResource = false, isSResource = false;

        if (_t == ast::builtins::buffer_type) {
            layoutDesc.type = OMEGASL_SHADER_BUFFER_DESC;
            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                isTResource = true;
                out << HLSL_BUFFER;
            } else {
                out << HLSL_RW_BUFFER;
            }
            out << "<";
            writeTypeName(cg.typeResolver->resolveTypeWithExpr(res_desc->typeExpr->args[0]),
                          res_desc->typeExpr->args[0]->pointer, out);
            out << ">";
        } else if (_t == ast::builtins::texture1d_type) {
            layoutDesc.type = OMEGASL_SHADER_TEXTURE1D_DESC;
            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                isTResource = true;
                out << HLSL_TEXTURE1D;
            } else {
                out << HLSL_RW_TEXTURE1D;
            }
            out << "<float4>";
        } else if (_t == ast::builtins::texture2d_type) {
            layoutDesc.type = OMEGASL_SHADER_TEXTURE2D_DESC;
            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                isTResource = true;
                out << HLSL_TEXTURE2D;
            } else {
                out << HLSL_RW_TEXTURE2D;
            }
            out << "<float4>";
        } else if (_t == ast::builtins::sampler1d_type) {
            isSResource = true;
            layoutDesc.type = res_desc->isStatic ? OMEGASL_SHADER_STATIC_SAMPLER1D_DESC
                                                 : OMEGASL_SHADER_SAMPLER1D_DESC;
            out << HLSL_SAMPLER;
        } else if (_t == ast::builtins::sampler2d_type) {
            isSResource = true;
            layoutDesc.type = res_desc->isStatic ? OMEGASL_SHADER_STATIC_SAMPLER2D_DESC
                                                 : OMEGASL_SHADER_SAMPLER2D_DESC;
            out << HLSL_SAMPLER;
        }

        out << " " << res_desc->name;

        auto fragmentSpace = [&]() -> const char * {
            return shader->shaderType == ast::ShaderDecl::Fragment ? "1" : "0";
        };

        if (isSResource && res_desc->isStatic) {
            /// Static-sampler descriptor is serialized into the layout
            /// metadata alongside the `: register(sN, spaceM);` line.
            layoutDesc.sampler_desc.filter = res_desc->staticSamplerDesc->filter;
            layoutDesc.sampler_desc.u_address_mode = res_desc->staticSamplerDesc->uAddressMode;
            layoutDesc.sampler_desc.v_address_mode = res_desc->staticSamplerDesc->vAddressMode;
            layoutDesc.sampler_desc.w_address_mode = res_desc->staticSamplerDesc->wAddressMode;
            layoutDesc.sampler_desc.max_anisotropy = res_desc->staticSamplerDesc->maxAnisotropy;
            layoutDesc.location = res_desc->registerNumber;
            out << ": register(s" << sResourceCount << ",space" << fragmentSpace() << ");" << std::endl;
            sResourceCount += 1;
        }

        if (!res_desc->isStatic) {
            out << ": register(";
            layoutDesc.location = res_desc->registerNumber;
            if (isTResource) {
                out << "t" << tResourceCount;
                layoutDesc.gpu_relative_loc = tResourceCount;
                ++tResourceCount;
            } else if (isSResource) {
                out << "s" << sResourceCount;
                layoutDesc.gpu_relative_loc = sResourceCount;
                ++sResourceCount;
            } else {
                out << "u" << uResourceCount;
                layoutDesc.gpu_relative_loc = uResourceCount;
                ++uResourceCount;
            }
            out << ",space" << fragmentSpace() << ");" << std::endl;
        }
    }

    OmegaCommon::StrRef HLSLTarget::discardStatement() { return "discard"; }

    void HLSLTarget::writeCast(CodeGen &cg, ast::TypeExpr *t, std::ostream &out) {
        writeTypeName(cg.typeResolver->resolveTypeWithExpr(t), t->pointer, out);
    }

    bool HLSLTarget::supportsPointerExpr() const { return true; }

    OmegaCommon::StrRef HLSLTarget::renameBuiltin(OmegaCommon::StrRef name) {
        /// HLSL's stdlib already uses `lerp` / `frac` / `atan2` natively,
        /// so the math builtins pass through. The `MAKE_*` constructors
        /// translate to HLSL's vector / matrix type names which double
        /// as constructors.
        if (name == BUILTIN_MAKE_FLOAT2)   return "float2";
        if (name == BUILTIN_MAKE_FLOAT3)   return "float3";
        if (name == BUILTIN_MAKE_FLOAT4)   return "float4";
        if (name == BUILTIN_MAKE_INT2)     return "int2";
        if (name == BUILTIN_MAKE_INT3)     return "int3";
        if (name == BUILTIN_MAKE_INT4)     return "int4";
        if (name == BUILTIN_MAKE_UINT2)    return "uint2";
        if (name == BUILTIN_MAKE_UINT3)    return "uint3";
        if (name == BUILTIN_MAKE_UINT4)    return "uint4";
        if (name == BUILTIN_MAKE_FLOAT2X2) return "float2x2";
        if (name == BUILTIN_MAKE_FLOAT3X3) return "float3x3";
        if (name == BUILTIN_MAKE_FLOAT4X4) return "float4x4";
        if (name == BUILTIN_MAKE_FLOAT2X3) return "float2x3";
        if (name == BUILTIN_MAKE_FLOAT2X4) return "float2x4";
        if (name == BUILTIN_MAKE_FLOAT3X2) return "float3x2";
        if (name == BUILTIN_MAKE_FLOAT3X4) return "float3x4";
        if (name == BUILTIN_MAKE_FLOAT4X2) return "float4x2";
        if (name == BUILTIN_MAKE_FLOAT4X3) return "float4x3";
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

    void HLSLTarget::emitShaderEntryHeader(CodeGen &cg,
                                           ast::ShaderDecl *_decl,
                                           omegasl_shader &shaderDesc,
                                           std::ostream &out) {
        /// 3. Stage decorators.
        if (_decl->shaderType == ast::ShaderDecl::Compute) {
            shaderDesc.threadgroupDesc.x = _decl->threadgroupDesc.x;
            shaderDesc.threadgroupDesc.y = _decl->threadgroupDesc.y;
            shaderDesc.threadgroupDesc.z = _decl->threadgroupDesc.z;
            out << "[numthreads(" << _decl->threadgroupDesc.x << "," << _decl->threadgroupDesc.y << ","
                << _decl->threadgroupDesc.z << ")]" << std::endl;
        } else if (_decl->shaderType == ast::ShaderDecl::Hull) {
            auto &td = _decl->tessDesc;
            out << "[domain(\""
                << (td.domain == ast::ShaderDecl::TessellationDesc::Triangle ? "tri" : "quad") << "\")]"
                << std::endl;
            const char *partStr = td.partitioning == ast::ShaderDecl::TessellationDesc::Integer ? "integer"
                                  : td.partitioning == ast::ShaderDecl::TessellationDesc::FractionalEven
                                      ? "fractional_even"
                                      : "fractional_odd";
            out << "[partitioning(\"" << partStr << "\")]" << std::endl;
            const char *topoStr =
                td.outputTopology == ast::ShaderDecl::TessellationDesc::TriangleCW    ? "triangle_cw"
                : td.outputTopology == ast::ShaderDecl::TessellationDesc::TriangleCCW ? "triangle_ccw"
                                                                                      : "line";
            out << "[outputtopology(\"" << topoStr << "\")]" << std::endl;
            out << "[outputcontrolpoints(" << td.outputControlPoints << ")]" << std::endl;
        } else if (_decl->shaderType == ast::ShaderDecl::Domain) {
            auto &td = _decl->tessDesc;
            out << "[domain(\""
                << (td.domain == ast::ShaderDecl::TessellationDesc::Triangle ? "tri" : "quad") << "\")]"
                << std::endl;
        }

        /// Function signature: <return> <name>(<params with attributes>)
        writeTypeName(cg.typeResolver->resolveTypeWithExpr(_decl->returnType),
                      _decl->returnType->pointer, out);
        out << " " << _decl->name;
        out << "(";
        for (auto p_it = _decl->params.begin(); p_it != _decl->params.end(); p_it++) {
            if (p_it != _decl->params.begin()) {
                out << ",";
            }

            writeTypeName(cg.typeResolver->resolveTypeWithExpr(p_it->typeExpr),
                          p_it->typeExpr->pointer, out);
            out << " " << p_it->name;
            if (p_it->attributeName.has_value()) {
                if (p_it->attributeName.value() == ATTRIBUTE_VERTEX_ID) {
                    shaderDesc.vertexShaderInputDesc.useVertexID = true;
                } else if (p_it->attributeName.value() == ATTRIBUTE_GLOBALTHREAD_ID) {
                    shaderDesc.computeShaderParamsDesc.useGlobalThreadID = true;
                } else if (p_it->attributeName.value() == ATTRIBUTE_LOCALTHREAD_ID) {
                    shaderDesc.computeShaderParamsDesc.useLocalThreadID = true;
                } else if (p_it->attributeName.value() == ATTRIBUTE_THREADGROUP_ID) {
                    shaderDesc.computeShaderParamsDesc.useThreadGroupID = true;
                }
                out << ":";
                writeAttribute(p_it->attributeName.value(), p_it->attributeIndex, out);
            }
        }
        out << ")";
        if (_decl->shaderType == ast::ShaderDecl::Fragment) {
            /// Bare-`float4` fragment returns get `SV_TARGET` here.
            /// When the fragment returns a struct, per-field semantics
            /// (`SV_TargetN`, `SV_Depth`, ...) carry the bindings, and
            /// no trailing semantic on the function is needed.
            auto retTy = cg.typeResolver->resolveTypeWithExpr(_decl->returnType);
            if (retTy == ast::builtins::float4_type) {
                out << " : SV_TARGET";
            }
        }
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
