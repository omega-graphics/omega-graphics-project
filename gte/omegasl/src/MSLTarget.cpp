#include "Target.h"
#include "AST.h"
#include "CodeGen.h"
#include <fstream>
#include <ostream>
#include <sstream>
#include <unordered_set>
#include <string>
#include <omega-common/multithread.h>

namespace omegasl {

#ifdef TARGET_METAL
    void compileMTLShader(void *mtl_device, unsigned length, const char *string, void **pDest);
#endif

    MSLTarget::MSLTarget(MetalCodeOpts &opts) : Target(Target::MSL), opts(opts) {}
    MSLTarget::~MSLTarget() = default;

    const char *MSLTarget::shaderFileExt(ast::ShaderDecl::Type /*stage*/) const {
        return ".metal";
    }

    bool MSLTarget::supportsStage(ast::ShaderDecl::Type stage,
                                  std::string &diagnosticOut) const {
        /// Metal has no direct hull-shader equivalent and the runtime has
        /// no tessellation pipeline plumbing yet. Reject hull/domain
        /// stages cleanly here so the shared SHADER_DECL handler aborts
        /// before opening the source file. See OmegaSL-Reference.md bug 3.
        if (stage == ast::ShaderDecl::Hull || stage == ast::ShaderDecl::Domain) {
            const char *kind = (stage == ast::ShaderDecl::Hull) ? "hull" : "domain";
            std::ostringstream ss;
            ss << "Metal backend does not support `" << kind
               << "` shaders. Tessellation on Metal requires a compute kernel for "
                  "patch factors plus a post-tessellation vertex stage; this is not "
                  "implemented yet (see OmegaSL-Reference.md bug 3).";
            diagnosticOut = ss.str();
            return false;
        }
        return true;
    }

    bool MSLTarget::compileShader(ast::ShaderDecl::Type stage,
                                  OmegaCommon::StrRef name,
                                  uint64_t /*requiredFeatures*/,
                                  const OmegaCommon::FS::Path &srcDir,
                                  const OmegaCommon::FS::Path &outDir) {
        /// Metal compiles every shader at the same MSL feature level —
        /// `half` / `short` are native and the per-feature `#extension`
        /// directive concept doesn't exist on this backend. The
        /// requiredFeatures bitfield is informational here.
        (void)stage;
        auto object_file = OmegaCommon::FS::Path(outDir).append(name).concat(".metallib").absPath();

        std::ostringstream out;
        out << "  -o " << object_file.c_str() << " "
            << OmegaCommon::FS::Path(srcDir).append(name).concat(shaderFileExt(stage)).absPath();

        auto metal_process = OmegaCommon::ChildProcess::OpenWithStdoutPipe(opts.metal_cmd, out.str().c_str());
        auto res = metal_process.wait();

        if (res != 0) {
            std::cerr << "error: metal compiler failed (exit " << res << ") for shader '"
                      << name.data() << "'" << std::endl;
            return false;
        }
        return true;
    }

    void MSLTarget::compileShaderRuntime(ast::ShaderDecl::Type /*stage*/,
                                         OmegaCommon::StrRef name,
                                         uint64_t /*requiredFeatures*/,
                                         const std::string &source,
                                         omegasl_shader &meta) {
#ifdef TARGET_METAL
        if (opts.mtl_device == nullptr) {
            return;
        }
        OmegaCommon::String shaderName{name.begin(), name.end()};
        {
            std::ofstream dump("/tmp/OmegaSL-" + shaderName + ".metal", std::ios::out | std::ios::trunc);
            if (dump.is_open()) {
                dump << source;
                dump.close();
            }
        }
        meta.data = nullptr;
        compileMTLShader(opts.mtl_device, source.size(), source.data(), &meta.data);
        if (meta.data == nullptr) {
            std::cout << "Runtime compile produced no Metal library for `" << shaderName << "`" << std::endl;
        }
#else
        (void)name;
        (void)source;
        (void)meta;
#endif
    }

    void MSLTarget::resetForNextShader() {
        bufferCount = 0;
        textureCount = 0;
        samplerCount = 0;
        paramIndex = 0;
        staticSamplers.clear();
    }

    void MSLTarget::emitStaticPreamble(std::ostream &/*out*/) {
        /// Phase 8d: static samplers are now flushed inside
        /// `emitShaderEntryBody` (after the opening `{`) so the
        /// in-function-body location matches the pre-Phase-8d byte
        /// output. The shared `emitResourcesAndFillLayout` still calls
        /// this hook after the resource loop; for MSL it must be a
        /// no-op because we are mid-parameter-list at that point.
    }

    void MSLTarget::emitResourceBinding(CodeGen &cg,
                                        ast::ResourceDecl *res_desc,
                                        ast::ShaderDecl */*shader*/,
                                        omegasl_shader_layout_desc_io_mode ioMode,
                                        std::ostream &out,
                                        omegasl_shader_layout_desc &layoutDesc) {
        using namespace ast;
        auto type_ = cg.typeResolver->resolveTypeWithExpr(res_desc->typeExpr);

        if (paramIndex != 0 && !res_desc->isStatic) {
            out << ",";
        }

        if (type_ == builtins::buffer_type) {
            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                out << "constant ";
            } else {
                out << "device ";
            }
        } else if (type_ == builtins::uniform_type) {
            /// §2.4 constant buffer — always read-only `constant` address space.
            out << "constant ";
        }

        omegasl_shader_layout_desc_type layoutDescType = OMEGASL_SHADER_BUFFER_DESC;
        bool isTexture = false, isBuffer = false, isSampler = false;

        auto writeSampler = [&]() {
            std::ostringstream s;
            s << "constexpr sampler " << res_desc->name << " = sampler(filter::";
            switch (res_desc->staticSamplerDesc->filter) {
                case OMEGASL_SHADER_SAMPLER_LINEAR_FILTER:                  s << "linear";  break;
                case OMEGASL_SHADER_SAMPLER_POINT_FILTER:                   s << "nearest"; break;
                case OMEGASL_SHADER_SAMPLER_MAX_ANISOTROPY_FILTER:
                case OMEGASL_SHADER_SAMPLER_MIN_ANISOTROPY_FILTER:          s << "linear";  break;
            }
            s << ",address::";
            switch (res_desc->staticSamplerDesc->uAddressMode) {
                case OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_WRAP:        s << "repeat";          break;
                case OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_MIRROR:      s << "mirrored_repeat"; break;
                case OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_MIRRORWRAP:  s << "mirrored_repeat"; break;
                case OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_CLAMPTOEDGE: s << "clamp_to_edge";   break;
            }
            s << ");" << std::endl;
            staticSamplers.push_back(s.str());
        };

        if (type_ == builtins::buffer_type) {
            isBuffer = true;
            writeTypeName(cg.typeResolver->resolveTypeWithExpr(res_desc->typeExpr->args[0]),
                          res_desc->typeExpr->args[0]->pointer, out);
            out << " *";
            layoutDescType = OMEGASL_SHADER_BUFFER_DESC;
        } else if (type_ == builtins::uniform_type) {
            /// §2.4 — `constant T& name [[buffer(N)]]`. The reference (vs the
            /// `*` a structured buffer uses) gives value-access `name.field`
            /// and no indexing. Shares the `[[buffer(N)]]` binding slot.
            isBuffer = true;
            writeTypeName(cg.typeResolver->resolveTypeWithExpr(res_desc->typeExpr->args[0]),
                          res_desc->typeExpr->args[0]->pointer, out);
            out << " &";
            layoutDescType = OMEGASL_SHADER_UNIFORM_DESC;
        } else if (type_ == builtins::texture1d_type) {
            isTexture = true;
            out << "texture1d<float,";
            layoutDescType = OMEGASL_SHADER_TEXTURE1D_DESC;
        } else if (type_ == builtins::texture2d_type) {
            isTexture = true;
            out << "texture2d<float,";
            layoutDescType = OMEGASL_SHADER_TEXTURE2D_DESC;
        } else if (type_ == builtins::texture3d_type) {
            isTexture = true;
            out << "texture3d<float,";
            layoutDescType = OMEGASL_SHADER_TEXTURE3D_DESC;
        } else if (type_ == builtins::texture1d_array_type) {
            isTexture = true;
            out << "texture1d_array<float,";
            layoutDescType = OMEGASL_SHADER_TEXTURE1D_ARRAY_DESC;
        } else if (type_ == builtins::texture2d_array_type) {
            isTexture = true;
            out << "texture2d_array<float,";
            layoutDescType = OMEGASL_SHADER_TEXTURE2D_ARRAY_DESC;
        } else if (type_ == builtins::texturecube_type) {
            isTexture = true;
            out << "texturecube<float,";
            layoutDescType = OMEGASL_SHADER_TEXTURECUBE_DESC;
        } else if (type_ == builtins::texturecube_array_type) {
            isTexture = true;
            out << "texturecube_array<float,";
            layoutDescType = OMEGASL_SHADER_TEXTURECUBE_ARRAY_DESC;
        } else if (type_ == builtins::texture2d_ms_type) {
            /// Multisample textures are read-only with explicit sample-index
            /// access. Sema rejects `write`/`sample`, so the access qualifier
            /// is forced to `read` regardless of `ioMode`. Emit the full
            /// template form here and skip the shared access-suffix block by
            /// keeping `isTexture` false.
            out << "texture2d_ms<float,access::read>";
            layoutDescType = OMEGASL_SHADER_TEXTURE2D_MS_DESC;
            /// Mirror the bookkeeping the shared `if(isTexture)` arms below
            /// would otherwise do.
            out << " " << res_desc->name;
            unsigned bindingMS = textureCount++;
            out << " [[texture(" << bindingMS << ")]]";
            layoutDesc.type = layoutDescType;
            layoutDesc.gpu_relative_loc = bindingMS;
            layoutDesc.io_mode = ioMode;
            layoutDesc.location = res_desc->registerNumber;
            paramIndex++;
            return;
        } else if (type_ == builtins::texture2d_ms_array_type) {
            out << "texture2d_ms_array<float,access::read>";
            layoutDescType = OMEGASL_SHADER_TEXTURE2D_MS_ARRAY_DESC;
            out << " " << res_desc->name;
            unsigned bindingMSA = textureCount++;
            out << " [[texture(" << bindingMSA << ")]]";
            layoutDesc.type = layoutDescType;
            layoutDesc.gpu_relative_loc = bindingMSA;
            layoutDesc.io_mode = ioMode;
            layoutDesc.location = res_desc->registerNumber;
            paramIndex++;
            return;
        } else if (type_ == builtins::samplercube_type) {
            isSampler = true;
            if (res_desc->isStatic) {
                layoutDescType = OMEGASL_SHADER_STATIC_SAMPLERCUBE_DESC;
                writeSampler();
            } else {
                /// Metal entry-function params need a type. Without `sampler`
                /// here the parameter list emits as ` <name>[[sampler(N)]]`
                /// — leading space, no type — which is invalid MSL.
                out << "sampler";
                layoutDescType = OMEGASL_SHADER_SAMPLERCUBE_DESC;
            }
        } else if (type_ == builtins::sampler1d_type) {
            isSampler = true;
            if (res_desc->isStatic) {
                layoutDescType = OMEGASL_SHADER_STATIC_SAMPLER1D_DESC;
                writeSampler();
            } else {
                out << "sampler";
                layoutDescType = OMEGASL_SHADER_SAMPLER1D_DESC;
            }
        } else if (type_ == builtins::sampler2d_type) {
            isSampler = true;
            if (res_desc->isStatic) {
                layoutDescType = OMEGASL_SHADER_STATIC_SAMPLER2D_DESC;
                writeSampler();
            } else {
                out << "sampler";
                layoutDescType = OMEGASL_SHADER_SAMPLER2D_DESC;
            }
        } else if (type_ == builtins::sampler3d_type) {
            isSampler = true;
            if (res_desc->isStatic) {
                layoutDescType = OMEGASL_SHADER_STATIC_SAMPLER3D_DESC;
                writeSampler();
            } else {
                out << "sampler";
                layoutDescType = OMEGASL_SHADER_SAMPLER3D_DESC;
            }
        }

        if (isTexture) {
            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                out << "access::sample>";
            } else if (ioMode == OMEGASL_SHADER_DESC_IO_INOUT) {
                out << "access::readwrite>";
            } else {
                out << "access::write>";
            }
        }

        if (!res_desc->isStatic) {
            out << " " << res_desc->name;
        }

        unsigned binding = 0;
        if (isTexture) {
            binding = textureCount;
        } else if (isBuffer) {
            binding = bufferCount;
        } else if (isSampler) {
            binding = samplerCount;
        }

        layoutDesc.type = layoutDescType;
        layoutDesc.gpu_relative_loc = binding;
        layoutDesc.io_mode = ioMode;
        layoutDesc.location = res_desc->registerNumber;

        if (isTexture) {
            out << "[[texture(" << textureCount << ")]]";
            ++textureCount;
        } else if (isBuffer) {
            out << "[[buffer(" << bufferCount << ")]]";
            ++bufferCount;
        } else if (isSampler && !res_desc->isStatic) {
            out << "[[sampler(" << samplerCount << ")]]";
            ++samplerCount;
        }

        ++paramIndex;
    }

    static const char defaultMetalHeaders[] = R"(// Warning! This file was generated by omegaslc
#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

)";

    const char *MSLTarget::shaderObjectFileExt(ast::ShaderDecl::Type /*stage*/) const {
        return ".metallib";
    }

    void MSLTarget::emitDefaultHeaders(CodeGen &/*cg*/, std::ostream &out) {
        /// MSL has `half`, `short`/`ushort`, and `long`/`ulong` (MSL 2.0+)
        /// in the standard library; no per-feature `#extension`-style
        /// preamble is needed. Runtime gating handles devices that
        /// can't run the resulting shader.
        out << defaultMetalHeaders;
    }

    void MSLTarget::emitStructDecl(CodeGen &cg, ast::StructDecl *_decl) {
        std::ostringstream out;
        out << "struct " << _decl->name << " {" << std::endl;
        for (auto &p : _decl->fields) {
            out << "    ";
            cg.writeTypeExpr(p.typeExpr, out);
            out << " " << p.name;
            if (p.attributeName.has_value()) {
                /// Bare `Color` and `TexCoord` are vertex→fragment varyings;
                /// MSL leaves them untagged on the struct. `Color(N)`,
                /// `Depth`, etc. get the `[[...]]` qualifier.
                bool isBareColor = (p.attributeName.value() == ATTRIBUTE_COLOR
                                    && !p.attributeIndex.has_value());
                bool isTexCoord  = (p.attributeName.value() == ATTRIBUTE_TEXCOORD);
                if (!isBareColor && !isTexCoord) {
                    out << "[[";
                    writeAttribute(p.attributeName.value(), p.attributeIndex, out);
                    out << "]]";
                }
            }
            out << ";" << std::endl;
        }
        out << "};" << std::endl;
        generatedStructs.insert(std::make_pair(std::string(_decl->name), out.str()));
    }

    void MSLTarget::emitShaderUsedStructs(CodeGen &cg, ast::ShaderDecl *_decl,
                                          std::ostream &out) {
        std::vector<std::string> used;
        cg.typeResolver->getStructsInFuncDecl(_decl, used);
        for (auto &t : used) {
            out << generatedStructs[t] << std::endl << std::endl;
        }
    }

    void MSLTarget::emitShaderEntryHeader(CodeGen &cg,
                                          ast::ShaderDecl *_decl,
                                          omegasl_shader &shadermap_entry,
                                          std::ostream &out) {
        cg.indentLevel = 0;

        shadermap_entry.name = new char[_decl->name.size() + 1];
        std::copy(_decl->name.begin(), _decl->name.end(), (char *)shadermap_entry.name);
        ((char *)shadermap_entry.name)[_decl->name.size()] = '\0';

        if (_decl->shaderType == ast::ShaderDecl::Vertex) {
            out << "vertex";
            shadermap_entry.type = OMEGASL_SHADER_VERTEX;
        } else if (_decl->shaderType == ast::ShaderDecl::Fragment) {
            out << "fragment";
            shadermap_entry.type = OMEGASL_SHADER_FRAGMENT;
        } else if (_decl->shaderType == ast::ShaderDecl::Compute) {
            out << "kernel";
            shadermap_entry.type = OMEGASL_SHADER_COMPUTE;
            shadermap_entry.threadgroupDesc.x = _decl->threadgroupDesc.x;
            shadermap_entry.threadgroupDesc.y = _decl->threadgroupDesc.y;
            shadermap_entry.threadgroupDesc.z = _decl->threadgroupDesc.z;
        } else if (_decl->shaderType == ast::ShaderDecl::Hull) {
            out << "kernel";
            shadermap_entry.type = OMEGASL_SHADER_HULL;
        } else if (_decl->shaderType == ast::ShaderDecl::Domain) {
            auto &td = _decl->tessDesc;
            out << "[[patch(" << (td.domain == ast::ShaderDecl::TessellationDesc::Triangle ? "triangle" : "quad")
                << ", " << td.outputControlPoints << ")]] vertex";
            shadermap_entry.type = OMEGASL_SHADER_DOMAIN;
        }

        out << " ";
        writeTypeName(cg.typeResolver->resolveTypeWithExpr(_decl->returnType),
                      _decl->returnType->pointer, out);
        out << " " << _decl->name << " ";
        out << "(";

        /// Resources interleave with params inside the parameter list. The
        /// shared helper drives `emitResourceBinding` (which tracks
        /// `paramIndex` so it knows when to emit a leading comma) and
        /// fills the per-binding layout descriptors. `emitStaticPreamble`
        /// is a no-op for MSL — static-sampler `constexpr sampler` lines
        /// gathered during resource emission are flushed in
        /// `emitShaderEntryBody` after the opening `{`.
        cg.emitResourcesAndFillLayout(_decl, shadermap_entry, out);

        if (!(_decl->params.empty()) && !(_decl->resourceMap.empty())) {
            out << ",";
        }

        for (auto p_it = _decl->params.begin(); p_it != _decl->params.end(); p_it++) {
            if (p_it != _decl->params.begin()) {
                out << ",";
            }

            auto &p = *p_it;

            writeTypeName(cg.typeResolver->resolveTypeWithExpr(p.typeExpr),
                          p.typeExpr->pointer, out);
            out << " " << p.name << " ";

            /// Only the rasterizer-struct param of a fragment carries
            /// `[[stage_in]]`. Per-fragment scalar inputs (FrontFacing,
            /// SampleIndex, ...) bring their own MSL attribute.
            if (_decl->shaderType == ast::ShaderDecl::Fragment
                && !p.attributeName.has_value()) {
                out << "[[stage_in]]";
            }

            if (p.attributeName.has_value()) {
                if (p.attributeName.value() == ATTRIBUTE_VERTEX_ID) {
                    shadermap_entry.vertexShaderInputDesc.useVertexID = true;
                } else if (p.attributeName.value() == ATTRIBUTE_GLOBALTHREAD_ID) {
                    shadermap_entry.computeShaderParamsDesc.useGlobalThreadID = true;
                } else if (p.attributeName.value() == ATTRIBUTE_THREADGROUP_ID) {
                    shadermap_entry.computeShaderParamsDesc.useThreadGroupID = true;
                }
                out << "[[";
                writeAttribute(p.attributeName.value(), p.attributeIndex, out);
                out << "]]";
            }
        }
        out << ")";
    }

    void MSLTarget::emitShaderEntryBody(CodeGen &cg,
                                        ast::ShaderDecl *_decl,
                                        omegasl_shader &/*meta*/,
                                        std::ostream &out) {
        out << "{" << std::endl;
        /// Flush the static-sampler `constexpr sampler ... = sampler(...);`
        /// lines that were gathered into `staticSamplers` during the
        /// resource loop. The trailing blank line matches the
        /// pre-Phase-8d byte output.
        for (auto &ss : staticSamplers) {
            out << ss;
        }
        out << std::endl;

        cg.indentLevel += 1;
        for (auto stmt : _decl->block->body) {
            for (unsigned l = cg.indentLevel; l != 0; l--) {
                out << "    ";
            }
            if (stmt->type == VAR_DECL || stmt->type == RETURN_DECL || stmt->type == IF_STMT
                || stmt->type == FOR_STMT || stmt->type == WHILE_STMT || stmt->type == BREAK_STMT
                || stmt->type == CONTINUE_STMT || stmt->type == DISCARD_STMT
                || stmt->type == SWITCH_STMT) {
                cg.generateDecl((ast::Decl *)stmt);
                if (stmt->type != IF_STMT && stmt->type != FOR_STMT && stmt->type != WHILE_STMT
                    && stmt->type != SWITCH_STMT) {
                    out << ";";
                }
                out << std::endl;
            } else {
                cg.generateExpr((ast::Expr *)stmt);
                out << ";" << std::endl;
            }
        }
        cg.indentLevel -= 1;
        out << "}" << std::endl;
    }

    /// §6.1 — MSL declares thread-group-shared memory inline at kernel
    /// scope (`threadgroup float tile[16][16]`), unlike HLSL/GLSL which
    /// hoist it to file scope. Return true to suppress the shared
    /// `generateDecl` emission; the caller appends the trailing `;`.
    bool MSLTarget::tryEmitVarDecl(CodeGen &cg, ast::VarDecl *_decl) {
        if (!_decl->isThreadgroup) {
            return false;
        }
        std::ostream &out = cg.getShaderOut();
        out << "threadgroup ";
        cg.writeTypeExpr(_decl->typeExpr, out);
        out << " ";
        writeIdentifier(_decl->spec.name, out);
        for (unsigned dim : _decl->typeExpr->arrayDims) {
            out << "[" << dim << "]";
        }
        return true;
    }

    OmegaCommon::StrRef MSLTarget::discardStatement() { return "discard_fragment()"; }

    void MSLTarget::writeCast(CodeGen &cg, ast::TypeExpr *t, std::ostream &out) {
        writeTypeName(cg.typeResolver->resolveTypeWithExpr(t), t->pointer, out);
    }

    /// §3.7 — Metal has no `out` / `inout` keyword. Both kinds are
    /// spelled as a `thread` address-space reference, which gives the
    /// callee a writable binding to the caller's storage. There is no
    /// write-only reference qualifier in MSL, so `out` and `inout`
    /// generate identical source — the difference exists only in the
    /// OmegaSL source for the author's intent. `in` (the default) uses
    /// pass-by-value, leaving the pre-3.7 spelling unchanged.
    void MSLTarget::writeFuncParam(CodeGen &cg,
                                   const ast::AttributedFieldDecl &param,
                                   std::ostream &out) {
        bool isByRef = (param.access == ast::AttributedFieldDecl::Out
                        || param.access == ast::AttributedFieldDecl::Inout);
        /// §3.6 — `const` param. Sema guarantees it never co-occurs with
        /// `out` / `inout`, so this only ever qualifies the by-value form
        /// (`const T name`) and never the `thread T&` reference.
        if (param.isConst) {
            out << "const ";
        }
        if (isByRef) {
            out << "thread ";
        }
        writeTypeName(cg.typeResolver->resolveTypeWithExpr(param.typeExpr),
                      param.typeExpr->pointer, out);
        if (isByRef) {
            out << "&";
        }
        out << " ";
        writeIdentifier(param.name, out);
    }

    bool MSLTarget::supportsPointerExpr() const { return true; }

    OmegaCommon::StrRef MSLTarget::renameBuiltin(OmegaCommon::StrRef name) {
        if (name == BUILTIN_LERP) return "mix";
        if (name == BUILTIN_FRAC) return "fract";
        if (name == BUILTIN_MAKE_FLOAT2)   return "float2";
        if (name == BUILTIN_MAKE_FLOAT3)   return "float3";
        if (name == BUILTIN_MAKE_FLOAT4)   return "float4";
        if (name == BUILTIN_MAKE_INT2)     return "int2";
        if (name == BUILTIN_MAKE_INT3)     return "int3";
        if (name == BUILTIN_MAKE_INT4)     return "int4";
        if (name == BUILTIN_MAKE_UINT2)    return "uint2";
        if (name == BUILTIN_MAKE_UINT3)    return "uint3";
        if (name == BUILTIN_MAKE_UINT4)    return "uint4";
        /// §4.1 / §4.2 — MSL spelling matches OmegaSL one-for-one
        /// since the type names are identical in MSL.
        if (name == BUILTIN_MAKE_HALF2)    return "half2";
        if (name == BUILTIN_MAKE_HALF3)    return "half3";
        if (name == BUILTIN_MAKE_HALF4)    return "half4";
        if (name == BUILTIN_MAKE_SHORT2)   return "short2";
        if (name == BUILTIN_MAKE_SHORT3)   return "short3";
        if (name == BUILTIN_MAKE_SHORT4)   return "short4";
        if (name == BUILTIN_MAKE_USHORT2)  return "ushort2";
        if (name == BUILTIN_MAKE_USHORT3)  return "ushort3";
        if (name == BUILTIN_MAKE_USHORT4)  return "ushort4";
        if (name == BUILTIN_MAKE_LONG2)    return "long2";
        if (name == BUILTIN_MAKE_LONG3)    return "long3";
        if (name == BUILTIN_MAKE_LONG4)    return "long4";
        if (name == BUILTIN_MAKE_ULONG2)   return "ulong2";
        if (name == BUILTIN_MAKE_ULONG3)   return "ulong3";
        if (name == BUILTIN_MAKE_ULONG4)   return "ulong4";
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

    /// MSL has no `degrees` or `radians` math builtin (their absence
    /// from the Metal stdlib is documented in the spec — every other
    /// platform spells them as ordinary functions). Inline the call as
    /// a multiplication by the matching π constant; the constants are
    /// `M_PI / 180` for `radians(x)` and `180 / M_PI` for `degrees(x)`.
    /// Vector arguments work without extra glue because scalar * vec
    /// broadcasts in MSL.
    bool MSLTarget::tryEmitBuiltinCall(CodeGen &cg,
                                       ast::CallExpr *_expr,
                                       OmegaCommon::StrRef name,
                                       std::ostream &out) {
        if (name == BUILTIN_DEGREES) {
            if (_expr->args.size() != 1) return false;
            out << "((";
            cg.generateExpr(_expr->args[0]);
            /// 180 / π — full double precision so the float-narrowing
            /// at use site picks the IEEE-best single-precision value.
            out << ") * 57.29577951308232)";
            return true;
        }
        if (name == BUILTIN_RADIANS) {
            if (_expr->args.size() != 1) return false;
            out << "((";
            cg.generateExpr(_expr->args[0]);
            /// π / 180.
            out << ") * 0.017453292519943295)";
            return true;
        }
        /// §6.2 — MSL spells both barriers as `threadgroup_barrier(<flag>)`;
        /// the memory-scope flag is injected (OmegaSL's barrier calls take
        /// no args). `mem_threadgroup` syncs execution + group memory;
        /// `mem_device` orders device memory (and, on MSL only, also syncs
        /// execution — see the portable-contract note in AST.def).
        if (name == BUILTIN_THREADGROUP_BARRIER) {
            out << "threadgroup_barrier(mem_flags::mem_threadgroup)";
            return true;
        }
        if (name == BUILTIN_DEVICE_BARRIER) {
            out << "threadgroup_barrier(mem_flags::mem_device)";
            return true;
        }
        return false;
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
        if(texTy == builtins::texture1d_array_type) return "uint";   /// inner coord only — layer is separate
        if(texTy == builtins::texture2d_array_type) return "uint2";
        if(texTy == builtins::texture2d_ms_type) return "uint2";
        if(texTy == builtins::texture2d_ms_array_type) return "uint2";
        return nullptr;
    }

    /// Resolve the texture-type of a `tex` argument for use in emit-side
    /// per-shape branching (cube/array/MS need explicit splitting in MSL).
    static ast::Type *metalResolveTextureType(CodeGen &cg, ast::Expr *texArg){
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
        return cg.typeResolver->resolveTypeWithExpr(texTypeExpr);
    }

    /// Emit the spatial-coord + layer-index split that Metal's `sample` /
    /// `gather` need for array and cube-array textures. The OmegaSL coord
    /// packs the layer into the trailing component (.y for 1D-array,
    /// .z for 2D-array, .w for cube-array); MSL takes them as separate
    /// arguments. For non-array textures, the coord is emitted as-is.
    /// Shared by `emitTextureSample`, `emitTextureSampleLOD`, ..., and
    /// `emitTextureGather`.
    static void emitMSLSampleCoord(CodeGen &cg, ast::Expr *coordArg, ast::Type *texTy, std::ostream &out){
        if(texTy == ast::builtins::texture1d_array_type){
            /// OmegaSL coord = float2(u, layer) → MSL `(u, uint(layer))`.
            out << "(";
            cg.generateExpr(coordArg);
            out << ").x,uint((";
            cg.generateExpr(coordArg);
            out << ").y)";
        } else if(texTy == ast::builtins::texture2d_array_type){
            /// OmegaSL coord = float3(uv, layer) → `((coord).xy, uint((coord).z))`.
            out << "(";
            cg.generateExpr(coordArg);
            out << ").xy,uint((";
            cg.generateExpr(coordArg);
            out << ").z)";
        } else if(texTy == ast::builtins::texturecube_array_type){
            /// OmegaSL coord = float4(xyz dir, w layer) → `((coord).xyz, uint((coord).w))`.
            out << "(";
            cg.generateExpr(coordArg);
            out << ").xyz,uint((";
            cg.generateExpr(coordArg);
            out << ").w)";
        } else {
            cg.generateExpr(coordArg);
        }
    }

    void MSLTarget::emitTextureSample(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// Metal splits the OmegaSL combined coord into separate (coord,
        /// layer) / (coord, sample_index) arguments for array / MS / cube
        /// types. The base coord stays as the OmegaSL float vector for the
        /// underlying shape; the trailing layer/face component is extracted
        /// via swizzle and cast to `uint`.
        auto *texTy = metalResolveTextureType(cg, _expr->args[1]);
        cg.generateExpr(_expr->args[1]);
        out << ".sample(";
        cg.generateExpr(_expr->args[0]);
        out << ",";
        emitMSLSampleCoord(cg, _expr->args[2], texTy, out);
        out << ")";
    }

    void MSLTarget::emitTextureSampleLOD(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// `tex.sample(s, coord [, layer], level(lod))`. The `level()` LOD
        /// argument is a Metal sample-option function call.
        auto *texTy = metalResolveTextureType(cg, _expr->args[1]);
        cg.generateExpr(_expr->args[1]);
        out << ".sample(";
        cg.generateExpr(_expr->args[0]);
        out << ",";
        emitMSLSampleCoord(cg, _expr->args[2], texTy, out);
        out << ",level(";
        cg.generateExpr(_expr->args[3]);
        out << "))";
    }

    void MSLTarget::emitTextureSampleBias(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// `tex.sample(s, coord [, layer], bias(b))`.
        auto *texTy = metalResolveTextureType(cg, _expr->args[1]);
        cg.generateExpr(_expr->args[1]);
        out << ".sample(";
        cg.generateExpr(_expr->args[0]);
        out << ",";
        emitMSLSampleCoord(cg, _expr->args[2], texTy, out);
        out << ",bias(";
        cg.generateExpr(_expr->args[3]);
        out << "))";
    }

    void MSLTarget::emitTextureSampleGrad(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// `tex.sample(s, coord [, layer], gradientNd(ddx, ddy))`. The MSL
        /// gradient option function is shape-specific:
        ///   2D / 2D-array → gradient2d
        ///   3D     → gradient3d
        ///   Cube / cube-array → gradientcube
        ///
        /// 1D textures have no equivalent — Apple GPUs don't store a
        /// mipmap pyramid for 1D textures, so MSL has no `gradient1d`
        /// function. Shaders that hit this path on a `texture1d` /
        /// `texture1d_array` trip `OMEGASL_FEATURE_BIT_TEXTURE1D_MIP_SAMPLE`
        /// in the FeatureScanner; the file-level `#requires` directive
        /// then routes the shader through the stub-emission path before
        /// reaching this emitter, so we never get here for a 1D texture
        /// when the gate is honored. If a shader reaches this path on
        /// 1D anyway (no `#requires`), the portability scanner has
        /// already warned and the resulting source will fail at the
        /// downstream metal compiler — that's the loud-fail we want.
        auto *texTy = metalResolveTextureType(cg, _expr->args[1]);
        const char *gradFn = "gradient2d";
        if(texTy == ast::builtins::texture1d_type
           || texTy == ast::builtins::texture1d_array_type){
            /// Emit the (invalid) `gradient1d` token so the downstream
            /// metal compiler produces a precise diagnostic pointing at
            /// the unsupported call. The portability scanner already
            /// warned at OmegaSL-source level upstream of this.
            gradFn = "gradient1d";
        } else if(texTy == ast::builtins::texture3d_type){
            gradFn = "gradient3d";
        } else if(texTy == ast::builtins::texturecube_type
                  || texTy == ast::builtins::texturecube_array_type){
            gradFn = "gradientcube";
        }
        cg.generateExpr(_expr->args[1]);
        out << ".sample(";
        cg.generateExpr(_expr->args[0]);
        out << ",";
        emitMSLSampleCoord(cg, _expr->args[2], texTy, out);
        out << "," << gradFn << "(";
        cg.generateExpr(_expr->args[3]);
        out << ",";
        cg.generateExpr(_expr->args[4]);
        out << "))";
    }

    void MSLTarget::emitTextureGather(CodeGen &cg, ast::CallExpr *_expr, int channel, std::ostream &out) {
        /// `tex.gather(s, coord [, layer] [, int2(0,0), component::C])`.
        /// The default `gather` (channel == -1) gathers the red component;
        /// gather{Red,Green,Blue,Alpha} use `component::x/y/z/w`. Metal's
        /// gather signature requires the `int2(0,0)` offset slot before the
        /// component selector — we emit `int2(0,0)` explicitly because
        /// OmegaSL doesn't expose offsets at this layer (Phase B).
        auto *texTy = metalResolveTextureType(cg, _expr->args[1]);
        cg.generateExpr(_expr->args[1]);
        out << ".gather(";
        cg.generateExpr(_expr->args[0]);
        out << ",";
        emitMSLSampleCoord(cg, _expr->args[2], texTy, out);
        if(channel >= 0){
            const char *comp;
            switch(channel){
                case 0: comp = "x"; break;
                case 1: comp = "y"; break;
                case 2: comp = "z"; break;
                case 3: comp = "w"; break;
                default: comp = "x"; break;
            }
            out << ",int2(0,0),component::" << comp;
        }
        out << ")";
    }

    void MSLTarget::emitTextureRead(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        auto *texTy = metalResolveTextureType(cg, _expr->args[0]);
        const char *coordCast = metalUintCoordTypeForTexture(cg, _expr->args[0]);
        cg.generateExpr(_expr->args[0]);
        out << ".read(";

        auto emitInnerCoord = [&](const char *cast) {
            if(cast){
                out << cast << "(";
                cg.generateExpr(_expr->args[1]);
                out << ")";
            } else {
                cg.generateExpr(_expr->args[1]);
            }
        };

        if(texTy == ast::builtins::texture1d_array_type){
            /// OmegaSL coord = int2(u, layer) → `(u_uint, layer_uint)`.
            out << "uint((";
            cg.generateExpr(_expr->args[1]);
            out << ").x),uint((";
            cg.generateExpr(_expr->args[1]);
            out << ").y)";
        } else if(texTy == ast::builtins::texture2d_array_type){
            /// OmegaSL coord = int3(uv, layer) → `(uv_uint2, layer_uint)`.
            out << "uint2((";
            cg.generateExpr(_expr->args[1]);
            out << ").xy),uint((";
            cg.generateExpr(_expr->args[1]);
            out << ").z)";
        } else if(texTy == ast::builtins::texture2d_ms_type){
            /// MS read: `(coord_uint2, sample_index_uint)`.
            emitInnerCoord(coordCast);
            out << ",uint(";
            cg.generateExpr(_expr->args[2]);
            out << ")";
        } else if(texTy == ast::builtins::texture2d_ms_array_type){
            /// MS-array read: OmegaSL coord = int3(uv, layer); 3rd arg = sample_index.
            out << "uint2((";
            cg.generateExpr(_expr->args[1]);
            out << ").xy),uint((";
            cg.generateExpr(_expr->args[1]);
            out << ").z),uint(";
            cg.generateExpr(_expr->args[2]);
            out << ")";
        } else {
            emitInnerCoord(coordCast);
        }
        out << ")";
    }

    void MSLTarget::emitTextureCalculateLOD(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// `tex.calculate_clamped_lod(s, spatialCoord)`. Metal's LOD query
        /// takes only the spatial coord; the array layer / cube-array face is
        /// dropped (`.xy` for 2D-array, `.xyz` for cube-array). 1D is rejected
        /// in Sema (Metal has no `calculate_*_lod` for `texture1d`). We use
        /// the clamped variant to match the advisory-LOD contract.
        auto *texTy = metalResolveTextureType(cg, _expr->args[1]);
        cg.generateExpr(_expr->args[1]);
        out << ".calculate_clamped_lod(";
        cg.generateExpr(_expr->args[0]);
        out << ",";
        if(texTy == ast::builtins::texture2d_array_type){
            out << "("; cg.generateExpr(_expr->args[2]); out << ").xy";
        } else if(texTy == ast::builtins::texturecube_array_type){
            out << "("; cg.generateExpr(_expr->args[2]); out << ").xyz";
        } else {
            cg.generateExpr(_expr->args[2]);
        }
        out << ")";
    }

    void MSLTarget::emitTextureGetDimensions(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// Metal exposes per-axis accessors emitted inline (no statement
        /// injection needed). `get_width/height/depth(lod)` take a mip level;
        /// `get_array_size()` does not. 1D textures have no mip pyramid, so
        /// their `get_width()` takes no lod — we drop it (the width is
        /// well-defined for a mip-less 1D texture regardless of lod).
        auto *texTy = metalResolveTextureType(cg, _expr->args[0]);
        std::string tex = cg.renderExprToString(_expr->args[0]);
        std::string lod = cg.renderExprToString(_expr->args[1]);
        if(texTy == ast::builtins::texture1d_type){
            out << "uint(" << tex << ".get_width())";
        } else if(texTy == ast::builtins::texture1d_array_type){
            out << "uint2(" << tex << ".get_width()," << tex << ".get_array_size())";
        } else if(texTy == ast::builtins::texture2d_type
                  || texTy == ast::builtins::texturecube_type){
            out << "uint2(" << tex << ".get_width(" << lod << ")," << tex << ".get_height(" << lod << "))";
        } else if(texTy == ast::builtins::texture2d_array_type
                  || texTy == ast::builtins::texturecube_array_type){
            out << "uint3(" << tex << ".get_width(" << lod << ")," << tex << ".get_height(" << lod << ")," << tex << ".get_array_size())";
        } else if(texTy == ast::builtins::texture3d_type){
            out << "uint3(" << tex << ".get_width(" << lod << ")," << tex << ".get_height(" << lod << ")," << tex << ".get_depth(" << lod << "))";
        } else {
            out << "uint(" << tex << ".get_width())";
        }
    }

    void MSLTarget::emitTextureWrite(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        auto *texTy = metalResolveTextureType(cg, _expr->args[0]);
        const char *coordCast = metalUintCoordTypeForTexture(cg, _expr->args[0]);
        cg.generateExpr(_expr->args[0]);
        out << ".write(";
        cg.generateExpr(_expr->args[2]);
        out << ",";
        if(texTy == ast::builtins::texture1d_array_type){
            /// `tex.write(val, uint(coord.x), uint(coord.y))`.
            out << "uint((";
            cg.generateExpr(_expr->args[1]);
            out << ").x),uint((";
            cg.generateExpr(_expr->args[1]);
            out << ").y)";
        } else if(texTy == ast::builtins::texture2d_array_type){
            out << "uint2((";
            cg.generateExpr(_expr->args[1]);
            out << ").xy),uint((";
            cg.generateExpr(_expr->args[1]);
            out << ").z)";
        } else if(coordCast){
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
        else if(attributeName == ATTRIBUTE_INPUT_COVERAGE
                || attributeName == ATTRIBUTE_OUTPUT_COVERAGE){
            /// MSL uses `[[sample_mask]]` for both directions; whether it
            /// lands on a fragment parameter (input) or a return-struct
            /// field (output) disambiguates.
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
        /// §4.1 16-bit family — Metal natively supports all of these
        /// since MSL 1.0 (`half`) / MSL 1.0 (`short`,`ushort`).
        else if(_t == builtins::half_type)   { out << "half"; }
        else if(_t == builtins::half2_type)  { out << "half2"; }
        else if(_t == builtins::half3_type)  { out << "half3"; }
        else if(_t == builtins::half4_type)  { out << "half4"; }
        else if(_t == builtins::short_type)  { out << "short"; }
        else if(_t == builtins::short2_type) { out << "short2"; }
        else if(_t == builtins::short3_type) { out << "short3"; }
        else if(_t == builtins::short4_type) { out << "short4"; }
        else if(_t == builtins::ushort_type) { out << "ushort"; }
        else if(_t == builtins::ushort2_type){ out << "ushort2"; }
        else if(_t == builtins::ushort3_type){ out << "ushort3"; }
        else if(_t == builtins::ushort4_type){ out << "ushort4"; }
        /// §4.2 64-bit ints — MSL 2.0+; runtime feature gate
        /// (OMEGASL_FEATURE_BIT_INT64) declines on devices/Metal
        /// versions that can't execute them.
        else if(_t == builtins::long_type)   { out << "long"; }
        else if(_t == builtins::long2_type)  { out << "long2"; }
        else if(_t == builtins::long3_type)  { out << "long3"; }
        else if(_t == builtins::long4_type)  { out << "long4"; }
        else if(_t == builtins::ulong_type)  { out << "ulong"; }
        else if(_t == builtins::ulong2_type) { out << "ulong2"; }
        else if(_t == builtins::ulong3_type) { out << "ulong3"; }
        else if(_t == builtins::ulong4_type) { out << "ulong4"; }
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
        else if(_t == builtins::sampler1d_type || _t == builtins::sampler2d_type || _t == builtins::sampler3d_type || _t == builtins::samplercube_type){
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
