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
        /// §2c — MSL now emits mesh source (`[[mesh]]`, `mesh<V, void,
        /// MaxV, MaxP, topology::X>` handle, scratch array + flush
        /// loop). The Hull/Domain rejection above still stands. See
        /// `gte/docs/Mesh-Shader-Implementation-Plan.md` → Phase 2c.
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
        /// `paramIndex` is intentionally NOT reset here. This function is
        /// called from `CodeGen::emitResourcesAndFillLayout` AFTER the
        /// mesh-stage pre-amble in `emitShaderEntryHeader` has already
        /// bumped `paramIndex` to account for the `__omegasl_mesh_output_handle`
        /// parameter — wiping it here would make the first non-static
        /// resource think it's the first item in the param list and skip
        /// the leading comma, producing `__omegasl_mesh_output_handleconstant
        /// MeshVertexIn * vertBuf...` (no comma between the handle and the
        /// first resource). `paramIndex` is now reset once per shader at
        /// the top of `emitShaderEntryHeader` instead, so the bump survives.
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
        } else if (type_ == builtins::push_constant_type) {
            /// §2.2/§10.2 push constant — bound via `setBytes:length:atIndex:`,
            /// which presents as `constant T&` exactly like a bound uniform
            /// buffer. Always the read-only `constant` address space.
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
        } else if (type_ == builtins::push_constant_type) {
            /// §2.2/§10.2 — `constant T& name [[buffer(N)]]`, byte-identical
            /// to a uniform: Metal has no separate push-constant construct,
            /// `setBytes:length:atIndex:` writes inline into the same buffer
            /// index space a bound buffer would use. The push-constant
            /// layout-desc type is what lets the runtime drive this slot via
            /// setBytes rather than setBuffer (Phase B).
            isBuffer = true;
            writeTypeName(cg.typeResolver->resolveTypeWithExpr(res_desc->typeExpr->args[0]),
                          res_desc->typeExpr->args[0]->pointer, out);
            out << " &";
            layoutDescType = OMEGASL_SHADER_PUSH_CONSTANT_DESC;
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
            cg.writeDeclTypeSuffix(p.typeExpr, out);
            if (p.attributeName.has_value()) {
                /// Wrap in `[[...]]` only when the attribute has an MSL
                /// spelling. Bare `Color` / `TexCoord` are vertex→fragment
                /// varyings MSL leaves untagged (writeAttribute emits nothing
                /// for them), and a backend-gated attribute that somehow
                /// reaches MSL without a mapping (e.g. `CullDistance`, which
                /// normally stubs the whole shader) also emits nothing —
                /// emitting a bare `[[]]` would be invalid MSL. Rendering to a
                /// temp keeps the output byte-identical for the tagged cases.
                std::ostringstream attrSS;
                writeAttribute(p.attributeName.value(), p.attributeIndex, attrSS);
                if (!attrSS.str().empty()) {
                    out << "[[" << attrSS.str() << "]]";
                }
            }
            /// §1.6 — interpolation qualifier as an MSL member attribute. Applies
            /// to varying fields (including the untagged bare Color/TexCoord ones,
            /// which is exactly where interpolation matters).
            switch (p.interp) {
                case ast::AttributedFieldDecl::Flat:          out << "[[flat]]"; break;
                case ast::AttributedFieldDecl::Centroid:      out << "[[centroid_perspective]]"; break;
                case ast::AttributedFieldDecl::Sample:        out << "[[sample_perspective]]"; break;
                case ast::AttributedFieldDecl::NoPerspective: out << "[[center_no_perspective]]"; break;
                default: break;
            }
            out << ";" << std::endl;
        }
        out << "};" << std::endl;
        generatedStructs.insert(std::make_pair(std::string(_decl->name), out.str()));
        /// §2c — record the StructDecl* so the mesh path in
        /// `emitShaderUsedStructs` can re-emit the vertex-output struct
        /// with inter-stage semantics (strip `[[color(N)]]` /
        /// `[[texcoord(N)]]` which are fragment-output decorations).
        structDeclMap[std::string(_decl->name)] = _decl;
    }

    void MSLTarget::emitShaderUsedStructs(CodeGen &cg, ast::ShaderDecl *_decl,
                                          std::ostream &out) {
        /// §2c — locate the mesh-vertex-output struct (the element type
        /// of the `out vertices` param). The cached `generatedStructs`
        /// text carries `[[color(N)]]` / `[[texcoord(N)]]` decorations
        /// for any indexed `Color(N)` / `TexCoord(N)` field — correct
        /// for a fragment output, invalid as a mesh-vertex-output
        /// decoration on Metal (Metal's mesh vertex outputs are
        /// untagged varyings except for `[[position]]`). Re-emit with
        /// those attributes stripped; every other struct keeps the
        /// cached text.
        ast::StructDecl *meshVertsStruct = nullptr;
        if (_decl->shaderType == ast::ShaderDecl::Mesh) {
            for (auto &p : _decl->params) {
                if (p.meshOutput == ast::AttributedFieldDecl::Vertices) {
                    auto sit = structDeclMap.find(std::string(p.typeExpr->name));
                    if (sit != structDeclMap.end()) meshVertsStruct = sit->second;
                    break;
                }
            }
        }
        std::vector<std::string> used;
        cg.typeResolver->getStructsInFuncDecl(_decl, used);
        for (auto &t : used) {
            if (meshVertsStruct && std::string(meshVertsStruct->name) == t) {
                out << "struct " << meshVertsStruct->name << " {" << std::endl;
                for (auto &f : meshVertsStruct->fields) {
                    out << "    ";
                    cg.writeTypeExpr(f.typeExpr, out);
                    out << " " << f.name;
                    cg.writeDeclTypeSuffix(f.typeExpr, out);
                    /// Strip `[[color(N)]]` / `[[texcoord(N)]]`. Every
                    /// other attribute (notably `Position →
                    /// [[position]]`) keeps the cached mapping —
                    /// `[[position]]` is the correct mesh vertex-output
                    /// decoration.
                    if (f.attributeName.has_value()) {
                        const auto &an = f.attributeName.value();
                        bool stripIndexed = (an == ATTRIBUTE_COLOR || an == ATTRIBUTE_TEXCOORD)
                                            && f.attributeIndex.has_value();
                        if (!stripIndexed) {
                            std::ostringstream attrSS;
                            writeAttribute(an, f.attributeIndex, attrSS);
                            if (!attrSS.str().empty()) {
                                out << "[[" << attrSS.str() << "]]";
                            }
                        }
                    }
                    switch (f.interp) {
                        case ast::AttributedFieldDecl::Flat:          out << "[[flat]]"; break;
                        case ast::AttributedFieldDecl::Centroid:      out << "[[centroid_perspective]]"; break;
                        case ast::AttributedFieldDecl::Sample:        out << "[[sample_perspective]]"; break;
                        case ast::AttributedFieldDecl::NoPerspective: out << "[[center_no_perspective]]"; break;
                        default: break;
                    }
                    out << ";" << std::endl;
                }
                out << "};" << std::endl << std::endl;
                continue;
            }
            out << generatedStructs[t] << std::endl << std::endl;
        }
    }

    void MSLTarget::emitShaderEntryHeader(CodeGen &cg,
                                          ast::ShaderDecl *_decl,
                                          omegasl_shader &shadermap_entry,
                                          std::ostream &out) {
        cg.indentLevel = 0;

        /// Reset the per-shader parameter-position counter at the very top,
        /// BEFORE the mesh-handle pre-amble below bumps it for the
        /// `__omegasl_mesh_output_handle` slot. The previous home for this
        /// reset was inside `resetForNextShader` (called from
        /// `CodeGen::emitResourcesAndFillLayout`), but that ran AFTER the
        /// mesh-handle bump and clobbered it — see the comment in
        /// `resetForNextShader` for the dropped-comma symptom this fixes.
        paramIndex = 0;

        shadermap_entry.name = new char[_decl->name.size() + 1];
        std::copy(_decl->name.begin(), _decl->name.end(), (char *)shadermap_entry.name);
        ((char *)shadermap_entry.name)[_decl->name.size()] = '\0';

        /// §2c — mesh stage pre-amble: locate the `out vertices` /
        /// `out indices` params, stamp the routing state every other
        /// mesh hook reads, and emit the per-shader `mesh<...>` type
        /// alias at file scope before the function decorator. The
        /// alias is what the `[[mesh]]` parameter is typed against;
        /// `using namespace metal` (in the default preamble) means we
        /// can spell `mesh<...>` / `topology::triangle` without the
        /// `metal::` qualifier. Reset to "not a mesh shader" defaults
        /// for every other stage.
        meshVertsParamName.clear();
        meshIndicesParamName.clear();
        meshVertsStructDecl = nullptr;
        meshMaxVertices = 0;
        meshMaxPrimitives = 0;
        if (_decl->shaderType == ast::ShaderDecl::Mesh) {
            for (auto &p : _decl->params) {
                if (p.meshOutput == ast::AttributedFieldDecl::Vertices) {
                    meshVertsParamName = p.name;
                    auto sit = structDeclMap.find(std::string(p.typeExpr->name));
                    if (sit != structDeclMap.end()) meshVertsStructDecl = sit->second;
                } else if (p.meshOutput == ast::AttributedFieldDecl::Indices) {
                    meshIndicesParamName = p.name;
                }
            }
            meshMaxVertices   = _decl->meshDesc.maxVertices;
            meshMaxPrimitives = _decl->meshDesc.maxPrimitives;
            meshTopology      = _decl->meshDesc.topology;
            const char *topoStr =
                (_decl->meshDesc.topology == ast::ShaderDecl::MeshDesc::Triangle) ? "triangle" : "line";
            /// `void` in the primitive-data slot — per-primitive output
            /// is a Phase 6 follow-up (see plan Open Decision 3).
            out << "using __omegasl_mesh_t_" << _decl->name << " = mesh<"
                << (meshVertsStructDecl ? meshVertsStructDecl->name : "void")
                << ", void, " << meshMaxVertices << ", " << meshMaxPrimitives
                << ", topology::" << topoStr << ">;" << std::endl;
        }

        if (_decl->shaderType == ast::ShaderDecl::Vertex) {
            out << "vertex";
            shadermap_entry.type = OMEGASL_SHADER_VERTEX;
        } else if (_decl->shaderType == ast::ShaderDecl::Fragment) {
            /// §1.5 — early depth/stencil attribute precedes the `fragment`
            /// function qualifier on MSL.
            if (_decl->earlyDepthStencil) {
                out << "[[early_fragment_tests]] ";
            }
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
        } else if (_decl->shaderType == ast::ShaderDecl::Mesh) {
            /// §2c — `[[mesh]]` is the function-attribute spelling, not
            /// a stage keyword. Stamp the threadgroup + mesh metadata
            /// onto `shadermap_entry` the same way every other
            /// thread-group stage does.
            out << "[[mesh]]";
            shadermap_entry.type = OMEGASL_SHADER_MESH;
            shadermap_entry.threadgroupDesc.x = _decl->threadgroupDesc.x;
            shadermap_entry.threadgroupDesc.y = _decl->threadgroupDesc.y;
            shadermap_entry.threadgroupDesc.z = _decl->threadgroupDesc.z;
            shadermap_entry.meshDesc.max_vertices   = meshMaxVertices;
            shadermap_entry.meshDesc.max_primitives = meshMaxPrimitives;
            shadermap_entry.meshDesc.topology       = static_cast<int>(meshTopology);
        }

        out << " ";
        writeTypeName(cg.typeResolver->resolveTypeWithExpr(_decl->returnType),
                      _decl->returnType->pointer, out);
        out << " " << _decl->name << " ";
        out << "(";

        /// §2c — mesh `[[mesh]]` parameter goes first in the list (it's
        /// the unified output handle that every per-vertex /
        /// per-primitive / per-index write routes through). Emit
        /// before resources/params so a non-empty resource map or
        /// non-empty param list correctly prepends a comma in the
        /// existing logic below. `paramIndex` is bumped so the shared
        /// `emitResourceBinding` knows to comma-separate the next
        /// non-static binding.
        bool meshHandleEmitted = false;
        if (_decl->shaderType == ast::ShaderDecl::Mesh) {
            out << "__omegasl_mesh_t_" << _decl->name
                << " __omegasl_mesh_output_handle";
            meshHandleEmitted = true;
            ++paramIndex;
        }

        /// Resources interleave with params inside the parameter list. The
        /// shared helper drives `emitResourceBinding` (which tracks
        /// `paramIndex` so it knows when to emit a leading comma) and
        /// fills the per-binding layout descriptors. `emitStaticPreamble`
        /// is a no-op for MSL — static-sampler `constexpr sampler` lines
        /// gathered during resource emission are flushed in
        /// `emitShaderEntryBody` after the opening `{`.
        cg.emitResourcesAndFillLayout(_decl, shadermap_entry, out);

        /// §2c — count visible params (mesh-output ones are suppressed)
        /// so we know whether the param list is non-empty and needs
        /// the leading comma after the resource block.
        unsigned visibleParams = 0;
        for (auto &p : _decl->params) {
            if (p.meshOutput == ast::AttributedFieldDecl::NotMeshOutput) ++visibleParams;
        }
        if (visibleParams > 0
            && (!(_decl->resourceMap.empty()) || meshHandleEmitted)) {
            out << ",";
        }

        bool firstParam = true;
        for (auto p_it = _decl->params.begin(); p_it != _decl->params.end(); p_it++) {
            auto &p = *p_it;
            /// §2c — `out vertices` / `out indices` have no presence in
            /// the MSL signature (they route through the mesh handle).
            if (p.meshOutput != ast::AttributedFieldDecl::NotMeshOutput) {
                continue;
            }
            if (!firstParam) {
                out << ",";
            }
            firstParam = false;

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

        /// §2c — mesh body prologue:
        ///   1. Declare the per-vertex scratch array
        ///      (`<VertStruct> __omegasl_verts_scratch[<max_v>];`). All
        ///      `verts[i].field = ...` writes route here via
        ///      `emitMemberExpr`; one flush loop at body close calls
        ///      `set_vertex(i, scratch[i])` for every slot.
        ///   2. Auto-emit `__omegasl_mesh_output_handle.set_primitive_count(<max_p>);`
        ///      when the user did NOT call `setMeshOutputs` themselves
        ///      — same suppression rule as GLSL 2a / HLSL 2b.
        ///      MSL's `set_primitive_count` takes only the primitive
        ///      count (vertex count is implicit, derived from the
        ///      highest `set_vertex` slot written), so the user's
        ///      `setMeshOutputs(nv, np)` lowers to
        ///      `__omegasl_mesh_output_handle.set_primitive_count(np)`
        ///      via `tryEmitBuiltinCall`; the `nv` arg is dropped by
        ///      design — see Mesh-Shader-Implementation-Plan.md →
        ///      Cross-Backend Differences.
        if (_decl->shaderType == ast::ShaderDecl::Mesh && meshVertsStructDecl) {
            for (unsigned i = 0; i < cg.indentLevel; i++) out << "    ";
            out << meshVertsStructDecl->name
                << " __omegasl_verts_scratch[" << meshMaxVertices << "];" << std::endl;
            if (!_decl->meshHasUserSetMeshOutputsCall) {
                for (unsigned i = 0; i < cg.indentLevel; i++) out << "    ";
                out << "__omegasl_mesh_output_handle.set_primitive_count("
                    << meshMaxPrimitives << ");" << std::endl;
            }
        }

        /// §2.3 Phase B / §5.2 — route each statement through
        /// `emitStatementLine` so a backend that queues pre-statements (the
        /// HLSL `getDimensions`/`frexp` injection, and the `inverse` adjugate
        /// expansion shared with HLSL) flushes them ahead of the statement.
        /// The hand-rolled loop this replaces called `generateDecl`/
        /// `generateExpr` directly and silently dropped any queued lines, so
        /// an injected builtin used at entry-body top level (not inside a
        /// nested block / user function) emitted a dangling temp reference.
        /// `emitStatementLine` owns indentation, the trailing `;`, and the
        /// block-statement check, so the output is byte-identical when nothing
        /// is queued.
        for (auto stmt : _decl->block->body) {
            cg.emitStatementLine(stmt);
        }

        /// §2c — mesh body epilogue: per-vertex flush loop. Writes every
        /// scratch-array slot to the mesh handle exactly once. Dynamic
        /// indices in the user's `verts[i].field = ...` writes are fine
        /// — the scratch array carries them and the loop flushes the
        /// whole `[0, max_v)` range. The flush count is the declared
        /// `max_vertices`; runtime per-shader narrowing (the equivalent
        /// of GLSL's `SetMeshOutputsEXT(nv, np)` first arg) is implicit
        /// on MSL via the highest slot written — extra `set_vertex`
        /// calls beyond what the user touched are allowed and just
        /// produce garbage vertices that aren't referenced.
        if (_decl->shaderType == ast::ShaderDecl::Mesh && meshVertsStructDecl) {
            for (unsigned i = 0; i < cg.indentLevel; i++) out << "    ";
            out << "for (uint __omegasl_mesh_flush_i = 0; "
                << "__omegasl_mesh_flush_i < " << meshMaxVertices
                << "; ++__omegasl_mesh_flush_i) {" << std::endl;
            for (unsigned i = 0; i < cg.indentLevel + 1; i++) out << "    ";
            out << "__omegasl_mesh_output_handle.set_vertex("
                << "__omegasl_mesh_flush_i, "
                << "__omegasl_verts_scratch[__omegasl_mesh_flush_i]);" << std::endl;
            for (unsigned i = 0; i < cg.indentLevel; i++) out << "    ";
            out << "}" << std::endl;
        }

        cg.indentLevel -= 1;
        out << "}" << std::endl;

        /// Reset per-shader mesh state — mirrors GLSL 2a / HLSL 2b so
        /// the next entry starts clean.
        if (_decl->shaderType == ast::ShaderDecl::Mesh) {
            meshVertsParamName.clear();
            meshIndicesParamName.clear();
            meshVertsStructDecl = nullptr;
            meshMaxVertices = 0;
            meshMaxPrimitives = 0;
        }
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
        cg.writeDeclTypeSuffix(_decl->typeExpr, out);
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

    void MSLTarget::emitMemberExpr(CodeGen &cg, ast::MemberExpr *expr, std::ostream &out) {
        /// §2c — mesh `out vertices` slot access reroute. When the LHS is
        /// `<vertsParam>[i]`, rewrite the base to
        /// `__omegasl_verts_scratch[i]` and keep the same `.field`
        /// member-access. MSL's mesh handle has no per-field accessor
        /// — only `set_vertex(i, T)` — so the user's per-field writes
        /// accumulate into the scratch array, and `emitShaderEntryBody`
        /// emits a single flush loop at body close that calls
        /// `set_vertex` for every slot. Everything else falls through
        /// to the default `lhs.field` emission inherited from
        /// `Target::emitMemberExpr`. Idx can be dynamic — the loop
        /// flushes the whole `[0, max_v)` range either way.
        if (!meshVertsParamName.empty()
            && expr->lhs && expr->lhs->type == INDEX_EXPR) {
            auto *_idx = (ast::IndexExpr *)expr->lhs;
            if (_idx->lhs && _idx->lhs->type == ID_EXPR
                && ((ast::IdExpr *)_idx->lhs)->id == meshVertsParamName) {
                out << "__omegasl_verts_scratch[";
                cg.generateExpr(_idx->idx_expr);
                out << "]." << expr->rhs_id;
                return;
            }
        }
        cg.generateExpr(expr->lhs);
        out << "." << expr->rhs_id;
    }

    bool MSLTarget::tryEmitBinaryExpr(CodeGen &cg, ast::BinaryExpr *expr, std::ostream &out) {
        /// §2c — mesh `out indices` slot write expansion. MSL's
        /// `set_index(slot, vertexIdx)` is per-slot, but OmegaSL writes
        /// a whole tuple per primitive: `tris[i] = uintK(a, b, c);`
        /// (K = 3 for triangle, 2 for line; point is Sema-rejected for
        /// portability — see Cross-Backend Differences). Detect
        /// `op == "="` whose lhs is `<indicesParam>[i]` and expand into
        ///   <pending>: `uintK __omegasl_mesh_idx_tmp = <rhs>;`
        ///   <pending>: `set_index(<i>*K + 0, tmp.x);`
        ///   <pending>: ...
        ///   <inline> : `set_index(<i>*K + (K-1), tmp.<last>)`
        /// `emitStatementLine` adds the trailing `;` on the inline part,
        /// matching how HLSL's `GetDimensions` queues pre-statements.
        if (expr->op != "=") return false;
        if (meshIndicesParamName.empty()) return false;
        if (!expr->lhs || expr->lhs->type != INDEX_EXPR) return false;
        auto *_idx = (ast::IndexExpr *)expr->lhs;
        if (!_idx->lhs || _idx->lhs->type != ID_EXPR) return false;
        if (((ast::IdExpr *)_idx->lhs)->id != meshIndicesParamName) return false;

        const unsigned K = (meshTopology == ast::ShaderDecl::MeshDesc::Triangle) ? 3u : 2u;
        const char *vecTy = (K == 3) ? "uint3" : "uint2";
        const char *lanes = "xyzw";

        std::string idxStr  = cg.renderExprToString(_idx->idx_expr);
        std::string rhsStr  = cg.renderExprToString(expr->rhs);
        std::string tmpName = "__omegasl_mesh_idx_tmp";

        cg.queuePendingStatement(std::string(vecTy) + " " + tmpName + " = " + rhsStr + ";");
        for (unsigned k = 0; k + 1 < K; ++k) {
            std::string s = "__omegasl_mesh_output_handle.set_index(("
                          + idxStr + ") * " + std::to_string(K) + " + "
                          + std::to_string(k) + ", " + tmpName + "."
                          + std::string(1, lanes[k]) + ");";
            cg.queuePendingStatement(s);
        }
        out << "__omegasl_mesh_output_handle.set_index(("
            << idxStr << ") * " << K << " + " << (K - 1)
            << ", " << tmpName << "." << lanes[K - 1] << ")";
        return true;
    }

    /// §5.3 — MSL spelling of an int/uint scalar or vector type, used by the
    /// firstbit normalization lowering.
    static const char *metalIntSpelling(bool isSigned, int arity){
        if(isSigned)
            switch(arity){ case 2: return "int2"; case 3: return "int3";
                           case 4: return "int4"; default: return "int"; }
        switch(arity){ case 2: return "uint2"; case 3: return "uint3";
                       case 4: return "uint4"; default: return "uint"; }
    }

    OmegaCommon::StrRef MSLTarget::renameBuiltin(OmegaCommon::StrRef name) {
        if (name == BUILTIN_LERP) return "mix";
        if (name == BUILTIN_FRAC) return "fract";
        /// §5.3 — MSL spells the integer ops differently; both are native
        /// on integer scalars and vectors (T → T), so a plain rename is
        /// enough and the operand-typed return contract holds.
        if (name == BUILTIN_COUNTBITS)   return "popcount";
        if (name == BUILTIN_REVERSEBITS) return "reverse_bits";
        /// §5.3 Phase B firstbithigh/firstbitlow are NOT plain renames on
        /// MSL — `clz`/`ctz` return zero-*counts*, not bit indices, and the
        /// normalized -1-on-zero result needs a conversion. Handled in
        /// `tryEmitBuiltinCall`.
        /// §5.4 — derivatives. MSL stdlib spells the basic ops `dfdx` /
        /// `dfdy` / `fwidth`. There are *no* coarse/fine variants in MSL,
        /// so `ddx_coarse`/`ddx_fine` silently widen to `dfdx`,
        /// `ddy_coarse`/`ddy_fine` to `dfdy`, and `fwidth_coarse`/
        /// `fwidth_fine` to `fwidth`. Coarse/fine is a hardware hint, not
        /// a hard contract — the unqualified form is whatever the GPU
        /// picks (often fine on modern Apple silicon).
        if (name == BUILTIN_DDX || name == BUILTIN_DDX_COARSE || name == BUILTIN_DDX_FINE)
            return "dfdx";
        if (name == BUILTIN_DDY || name == BUILTIN_DDY_COARSE || name == BUILTIN_DDY_FINE)
            return "dfdy";
        if (name == BUILTIN_FWIDTH_COARSE || name == BUILTIN_FWIDTH_FINE)
            return "fwidth";
        /// `fwidth` itself passes through unchanged on MSL.
        if (name == BUILTIN_MAKE_FLOAT2)   return "float2";
        if (name == BUILTIN_MAKE_FLOAT3)   return "float3";
        if (name == BUILTIN_MAKE_FLOAT4)   return "float4";
        if (name == BUILTIN_MAKE_BOOL2)    return "bool2";
        if (name == BUILTIN_MAKE_BOOL3)    return "bool3";
        if (name == BUILTIN_MAKE_BOOL4)    return "bool4";
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
        /// §2a follow-up / §2c — mesh-shader runtime output count. MSL is
        /// the outlier here (see Mesh-Shader-Implementation-Plan.md →
        /// Cross-Backend Differences → Active-count "set outputs"
        /// call): the `mesh<...>` handle exposes ONLY
        /// `set_primitive_count(uint)`. The vertex count is implicit,
        /// derived from the highest `set_vertex(i, …)` slot the kernel
        /// writes, so the `nv` argument from OmegaSL's
        /// `setMeshOutputs(nv, np)` is dropped here by design — not an
        /// oversight. `__omegasl_mesh_output_handle` is the live MSL
        /// mesh-handle param name emitted by `emitShaderEntryHeader`
        /// in Phase 2c — the placeholder note this comment carried
        /// pre-2c was resolved by keeping the same name when the
        /// handle was materialized.
        if (name == BUILTIN_SET_MESH_OUTPUTS) {
            if (_expr->args.size() != 2) return false;
            out << "__omegasl_mesh_output_handle.set_primitive_count(";
            cg.generateExpr(_expr->args[1]);
            out << ")";
            return true;
        }
        /// §5.3 Phase B — firstbithigh / firstbitlow normalization on MSL.
        /// The operand is cast to its unsigned form so signed and unsigned
        /// operands agree (the index of the raw bit pattern). `intN(...)` /
        /// `int` is the result type Sema assigned.
        ///   firstbithigh(x) → (31 - intN(clz(uintN(x))))
        ///     clz(0) == bitwidth (32), so 31 - 32 == -1 for zero. ✔
        ///   firstbitlow(x)  → select(intN(ctz(uintN(x))), intN(-1), x == 0)
        ///     ctz(0) == 32 on MSL, which is not -1, so the zero case is
        ///     fixed explicitly with a component-wise `select`.
        if (name == BUILTIN_FIRSTBITHIGH || name == BUILTIN_FIRSTBITLOW) {
            if (_expr->args.size() != 1) return false;
            auto *ty = cg.typeResolver->resolveTypeWithExpr(_expr->args[0]->resolvedType);
            bool isSigned; int arity;
            if (!cg.intOperandShape(ty, isSigned, arity)) return false;
            const char *uSpell = metalIntSpelling(false, arity); // uint / uintN
            const char *iSpell = metalIntSpelling(true, arity);  // int / intN
            std::string a = cg.renderExprToString(_expr->args[0]);
            std::string u = std::string(uSpell) + "(" + a + ")";
            if (name == BUILTIN_FIRSTBITHIGH) {
                out << "(31 - " << iSpell << "(clz(" << u << ")))";
            } else {
                out << "select(" << iSpell << "(ctz(" << u << ")), "
                    << iSpell << "(-1), " << u << " == " << uSpell << "(0))";
            }
            return true;
        }
        /// §5.3 Phase C — MSL `extract_bits`/`insert_bits` are native
        /// (scalar + vector, signed + unsigned). The offset/bits args are
        /// `uint` in the MSL signature, so wrap them in `uint(...)`. The
        /// value/base/insert operands are cast to the operand-type spelling:
        /// a bare numeric literal (e.g. `0xFFu` → `255`) emits without its
        /// unsigned-ness, which makes Metal's overload set ambiguous — the
        /// explicit cast pins the overload.
        if (name == BUILTIN_BITFIELD_EXTRACT || name == BUILTIN_BITFIELD_INSERT) {
            bool isSigned; int arity;
            auto *ty = cg.typeResolver->resolveTypeWithExpr(_expr->args[0]->resolvedType);
            if (!cg.intOperandShape(ty, isSigned, arity)) return false;
            const char *vty = metalIntSpelling(isSigned, arity);
            if (name == BUILTIN_BITFIELD_EXTRACT) {
                if (_expr->args.size() != 3) return false;
                out << "extract_bits(" << vty << "("; cg.generateExpr(_expr->args[0]);
                out << "), uint("; cg.generateExpr(_expr->args[1]);
                out << "), uint("; cg.generateExpr(_expr->args[2]);
                out << "))";
            } else {
                if (_expr->args.size() != 4) return false;
                out << "insert_bits(" << vty << "("; cg.generateExpr(_expr->args[0]);
                out << "), " << vty << "("; cg.generateExpr(_expr->args[1]);
                out << "), uint("; cg.generateExpr(_expr->args[2]);
                out << "), uint("; cg.generateExpr(_expr->args[3]);
                out << "))";
            }
            return true;
        }
        /// §5.2 — Metal has no matrix `inverse`; lower to an injected
        /// adjugate expansion (shared with HLSL).
        if (name == BUILTIN_INVERSE) {
            return cg.emitInverseCall(_expr, out);
        }
        /// §5.2 — Metal has no `lessThan`/`equal`/… functions; component-wise
        /// compare is the `(a OP b)` operator form (shared with HLSL).
        if (cg.emitVectorCompare(_expr, name, out)) {
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
        else if(attributeName == ATTRIBUTE_CLIP_DISTANCE){
            /// §1.7 — Metal supports clip distance. CullDistance has no Metal
            /// equivalent and is gated by OMEGASL_FEATURE_BIT_CULL_DISTANCE
            /// (the shader stubs out on MSL), so it never reaches here.
            out << "clip_distance";
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

        /// §12.2 follow-up — Metal has no integer matrix type; lower to an
        /// array of column vectors (`int4 m[C]`). The declarator site appends
        /// the `[C]` array dimension via `CodeGen::writeDeclTypeSuffix`.
        {
            bool isSigned; unsigned cols, rows;
            if(CodeGen::integerMatrixShape(_t, isSigned, cols, rows)){
                out << (isSigned ? "int" : "uint") << rows;
                return;
            }
        }
        if(_t == builtins::void_type){
            out << "void";
        }
        else if(_t == builtins::bool_type){
            out << "bool";
        }
        else if(_t == builtins::bool2_type){ out << "bool2"; }
        else if(_t == builtins::bool3_type){ out << "bool3"; }
        else if(_t == builtins::bool4_type){ out << "bool4"; }
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
