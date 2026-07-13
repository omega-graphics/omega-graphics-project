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

    /// §5 — the synthesized `mesh_grid_properties` parameter on an `[[object]]`
    /// entry. MSL has no free-function child-dispatch call: launching the mesh
    /// grid is a method on this handle, so the object function must take one.
    /// Named in exactly two places (the signature in `emitShaderEntryHeader` and
    /// the `dispatchMesh` lowering in `tryEmitBuiltinCall`), which is precisely
    /// why it is a named constant — a typo in either would compile to a
    /// use-of-undeclared-identifier only Metal's toolchain would catch, and only
    /// on a macOS host.
    static constexpr const char *MSL_MESH_GRID_HANDLE = "__omegasl_mesh_grid";

    MSLTarget::MSLTarget(MetalCodeOpts &opts) : Target(Target::MSL), opts(opts) {}
    MSLTarget::~MSLTarget() = default;

    const char *MSLTarget::shaderFileExt(ast::ShaderDecl::Type /*stage*/) const {
        return ".metal";
    }

    bool MSLTarget::supportsStage(ast::ShaderDecl::Type /*stage*/,
                                  std::string &/*diagnosticOut*/) const {
        /// §16 — Metal tessellation now lands: a `hull` lowers to a compute
        /// kernel (per-CP output + a tess-factor epilogue via the patchfn)
        /// and a `domain` lowers to a `[[patch(...)]]` post-tessellation
        /// vertex consuming `patch_control_point<CP>` + `[[position_in_patch]]`.
        /// The prior Hull/Domain rejection (OmegaSL-Reference.md bug 3) is
        /// resolved. §2c mesh source (`[[mesh]]`, scratch array + flush loop)
        /// is likewise supported, so every stage is emittable on MSL.
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
                                        ast::ShaderDecl *shader,
                                        omegasl_shader_layout_desc_io_mode ioMode,
                                        std::ostream &out,
                                        omegasl_shader_layout_desc &layoutDesc) {
        using namespace ast;
        auto type_ = cg.typeResolver->resolveTypeWithExpr(res_desc->typeExpr);

        if (paramIndex != 0 && !res_desc->isStatic) {
            out << ",";
        }

        /// §16 — a `domain` shader's `[in]` control-point buffer is not a
        /// plain buffer read but the post-tessellation vertex's stage input:
        /// `patch_control_point<CP> name [[stage_in]]`, so the body's
        /// `name[i]` indexes a per-patch control point. It consumes no buffer
        /// slot (stage_in is not a `[[buffer(N)]]` binding). Runtime binding
        /// of this input is Phase E; the layout desc is recorded as a buffer
        /// for now so the shader-map stays well-formed.
        if (shader && shader->shaderType == ast::ShaderDecl::Domain
            && type_ == builtins::buffer_type && ioMode == OMEGASL_SHADER_DESC_IO_IN) {
            out << "patch_control_point<";
            writeTypeName(cg.typeResolver->resolveTypeWithExpr(res_desc->typeExpr->args[0]),
                          res_desc->typeExpr->args[0]->pointer, out);
            out << "> " << res_desc->name << " [[stage_in]]";
            layoutDesc.type = OMEGASL_SHADER_BUFFER_DESC;
            layoutDesc.gpu_relative_loc = 0;
            layoutDesc.io_mode = ioMode;
            layoutDesc.location = res_desc->registerNumber;
            ++paramIndex;
            return;
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
        } else if (type_ == builtins::acceleration_structure_type) {
            /// Inline ray tracing (Raytracing plan §2.3). A TLAS is a
            /// value-typed `acceleration_structure<instancing>` (from
            /// <metal_raytracing>, in scope via `using namespace
            /// metal::raytracing` in emitDefaultHeaders) bound at a buffer index
            /// through `setAccelerationStructure:atBufferIndex:`. It rides the
            /// buffer index space (isBuffer) but takes no address-space prefix —
            /// it is a value, not a `constant`/`device` pointer/reference.
            isBuffer = true;
            out << "acceleration_structure<instancing>";
            layoutDescType = OMEGASL_SHADER_ACCELERATION_STRUCTURE_DESC;
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

        /// A STATIC sampler contributes no entry-function parameter (Phase 8d
        /// flushes it as an in-body `constexpr sampler`), so it must not
        /// advance the parameter-position counter — otherwise the next real
        /// resource emits a stray leading comma (`fragFunc (,texture2d...`).
        /// Latent since the Phase 8d refactor: the shader libs consuming
        /// static samplers were never rebuilt because `add_omegasl_lib`
        /// didn't depend on the omegaslc binary (fixed alongside this).
        if (!res_desc->isStatic) {
            ++paramIndex;
        }
    }

    static const char defaultMetalHeaders[] = R"(// Warning! This file was generated by omegaslc
#include <metal_stdlib>
#include <simd/simd.h>
#include <metal_tessellation>

using namespace metal;

)";

    const char *MSLTarget::shaderObjectFileExt(ast::ShaderDecl::Type /*stage*/) const {
        return ".metallib";
    }

    /// Inline ray tracing (Raytracing plan §1.2/§2.3). Fixed MSL text for the
    /// `Ray` / `RayHit` builtin structs (`float3`/`float2` spellings, identical
    /// to HLSL). The trailing `};\n` matches what `emitStructDecl` caches so the
    /// used-struct emission path spells them identically. `Ray` (capitalized)
    /// does not collide with Metal's `metal::raytracing::ray`.
    static const char mslRayStruct[] =
        "struct Ray {\n"
        "    float3 origin;\n"
        "    float3 direction;\n"
        "    float tmin;\n"
        "    float tmax;\n"
        "};\n";
    static const char mslRayHitStruct[] =
        "struct RayHit {\n"
        "    bool committed;\n"
        "    float t;\n"
        "    uint primitiveIndex;\n"
        "    uint instanceIndex;\n"
        "    float2 barycentrics;\n"
        "};\n";

    void MSLTarget::emitDefaultHeaders(CodeGen &cg, std::ostream &out) {
        /// MSL has `half`, `short`/`ushort`, and `long`/`ulong` (MSL 2.0+)
        /// in the standard library; no per-feature `#extension`-style
        /// preamble is needed. Runtime gating handles devices that
        /// can't run the resulting shader.
        out << defaultMetalHeaders;
        /// Inline ray tracing (Raytracing plan §2.3) — `<metal_raytracing>`
        /// supplies `ray`, `intersector`, `intersection_result`,
        /// `acceleration_structure`, and the `triangle_data`/`instancing` tags.
        /// Bring the `metal::raytracing` namespace into scope so the emitted
        /// lowering and resource types can be spelled unqualified. Pre-seed the
        /// `Ray`/`RayHit` struct text so the used-struct emission spells them
        /// (they resolve `builtin = false`). Gated on the RT feature bit.
        if (cg.fileRequiredFeatures & OMEGASL_FEATURE_BIT_RAYTRACING) {
            out << "#include <metal_raytracing>\n";
            out << "using namespace metal::raytracing;\n\n";
            generatedStructs.insert(std::make_pair(OmegaCommon::String("Ray"),
                                                   OmegaCommon::String(mslRayStruct)));
            generatedStructs.insert(std::make_pair(OmegaCommon::String("RayHit"),
                                                   OmegaCommon::String(mslRayHitStruct)));
        }
    }

    bool MSLTarget::tryEmitReturnDecl(CodeGen &cg, ast::ReturnDecl *decl) {
        /// §16 — inside a hull kernel, `return <expr>;` is not a real return:
        /// it stores the per-control-point result into the `out` buffer at the
        /// control-point index (`<hullOut>[<vid>] = <expr>`), so the tess-factor
        /// epilogue appended after the body still runs. `emitStatementLine`
        /// owns the trailing `;`, so emit the assignment without one. Every
        /// other stage (and a valueless `return;`) falls through to the default
        /// emission.
        if (!inHullShader || decl->expr == nullptr) {
            return false;
        }
        auto &out = cg.shaderOutStream();
        out << hullOutBufferName << "[" << hullVidName << "] = ";
        cg.generateExpr(decl->expr);
        return true;
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

        /// §16 — a `domain` shader consumes control points via
        /// `patch_control_point<CP> [[stage_in]]`, and Metal only accepts a
        /// struct as a patch control point (`__is_metal_patch_control_point_struct`)
        /// when its fields carry `[[attribute(N)]]`. The control-point struct
        /// is the element of the domain's `[in]` buffer; re-emit it with
        /// positional stage-in attributes. (The same struct emits plain in the
        /// hull's own TU, where it is a `constant CP*` buffer read.)
        std::string domainCPStruct;
        if (_decl->shaderType == ast::ShaderDecl::Domain) {
            for (auto &r : _decl->resourceMap) {
                if (r.access != ast::ShaderDecl::ResourceMapDesc::In) continue;
                auto it = cg.resourceStore.find(r.name);
                if (it == cg.resourceStore.end()) continue;
                auto *rd = *it;
                auto rtype = cg.typeResolver->resolveTypeWithExpr(rd->typeExpr);
                if (rtype == ast::builtins::buffer_type && !rd->typeExpr->args.empty()) {
                    domainCPStruct = std::string(rd->typeExpr->args[0]->name);
                    break;
                }
            }
        }

        std::vector<std::string> used;
        cg.typeResolver->getStructsInFuncDecl(_decl, used);
        for (auto &t : used) {
            if (!domainCPStruct.empty() && domainCPStruct == t) {
                auto sit = structDeclMap.find(t);
                if (sit != structDeclMap.end()) {
                    auto *sd = sit->second;
                    out << "struct " << sd->name << " {" << std::endl;
                    unsigned attrIdx = 0;
                    for (auto &f : sd->fields) {
                        out << "    ";
                        cg.writeTypeExpr(f.typeExpr, out);
                        out << " " << f.name;
                        cg.writeDeclTypeSuffix(f.typeExpr, out);
                        out << "[[attribute(" << attrIdx++ << ")]]";
                        out << ";" << std::endl;
                    }
                    out << "};" << std::endl << std::endl;
                    continue;
                }
            }
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

        /// §16 — hull-kernel state setup. Reset for every stage; populate
        /// for a `hull`. Sema has already guaranteed the patchfn exists,
        /// takes no params, returns a factor struct with the right slot
        /// counts for the domain, and that there is exactly one `out`
        /// buffer — so the lookups below are total.
        inHullShader = false;
        hullOutBufferName.clear();
        hullVidName.clear();
        hullPatchFnName.clear();
        hullEdgeFieldName.clear();
        hullInsideFieldName.clear();
        hullEdgeIsArray = hullInsideIsArray = false;
        if (_decl->shaderType == ast::ShaderDecl::Hull) {
            inHullShader = true;
            auto &td = _decl->tessDesc;
            hullControlPoints = td.outputControlPoints;
            hullDomainIsTri = (td.domain == ast::ShaderDecl::TessellationDesc::Triangle);
            hullPatchFnName = td.patchFn;
            for (auto &r : _decl->resourceMap) {
                if (r.access == ast::ShaderDecl::ResourceMapDesc::Out) { hullOutBufferName = r.name; break; }
            }
            for (auto &p : _decl->params) {
                if (p.attributeName.has_value() && p.attributeName.value() == ATTRIBUTE_VERTEX_ID) {
                    hullVidName = p.name; break;
                }
            }
            if (hullVidName.empty()) { hullVidName = "__osl_cpid"; }
            /// Resolve the patchfn's return struct → the factor field names
            /// the epilogue reads (`__pc.<edge>[i]` / `__pc.<inside>[j]`).
            for (auto *fd : cg.userFuncDecls) {
                if (fd->name != hullPatchFnName) continue;
                auto sit = cg.structDeclsByName.find(fd->returnType->name);
                if (sit != cg.structDeclsByName.end()) {
                    for (auto &f : sit->second->fields) {
                        if (!f.attributeName.has_value()) continue;
                        if (f.attributeName.value() == ATTRIBUTE_TESS_FACTOR) {
                            hullEdgeFieldName = f.name;
                            hullEdgeIsArray = !f.typeExpr->arrayDims.empty();
                        } else if (f.attributeName.value() == ATTRIBUTE_INSIDE_TESS_FACTOR) {
                            hullInsideFieldName = f.name;
                            hullInsideIsArray = !f.typeExpr->arrayDims.empty();
                        }
                    }
                }
                break;
            }
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
            fillTessellationDesc(_decl->tessDesc, shadermap_entry.tessellationDesc);
            /// §16 Phase E — the hull lowers to a compute kernel dispatched one
            /// thread per patch. Give it a valid (non-zero) threadgroup size so
            /// the runtime's `makeComputePipelineState`
            /// (`maxTotalThreadsPerThreadgroup`) and `dispatchThreads`
            /// (`threadsPerThreadgroup`) are well-formed. 32×1×1 is safe on every
            /// Metal GPU; tuning per-patch dispatch occupancy is a follow-up.
            shadermap_entry.threadgroupDesc.x = 32;
            shadermap_entry.threadgroupDesc.y = 1;
            shadermap_entry.threadgroupDesc.z = 1;
        } else if (_decl->shaderType == ast::ShaderDecl::Domain) {
            auto &td = _decl->tessDesc;
            out << "[[patch(" << (td.domain == ast::ShaderDecl::TessellationDesc::Triangle ? "triangle" : "quad")
                << ", " << td.outputControlPoints << ")]] vertex";
            shadermap_entry.type = OMEGASL_SHADER_DOMAIN;
            fillTessellationDesc(td, shadermap_entry.tessellationDesc);
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
        } else if (_decl->shaderType == ast::ShaderDecl::Amplification) {
            /// §5 — Metal calls the amplification stage the OBJECT stage.
            /// `[[object]]` is a function attribute, same spelling shape as
            /// `[[mesh]]`. It dispatches like compute, so its `[numthreads]`
            /// equivalent rides `threadgroupDesc` exactly as compute's does —
            /// the runtime reads that field for `threadsPerObjectThreadgroup:`.
            out << "[[object]]";
            shadermap_entry.type = OMEGASL_SHADER_AMPLIFICATION;
            shadermap_entry.threadgroupDesc.x = _decl->threadgroupDesc.x;
            shadermap_entry.threadgroupDesc.y = _decl->threadgroupDesc.y;
            shadermap_entry.threadgroupDesc.z = _decl->threadgroupDesc.z;
        }

        out << " ";
        /// §16 — a hull lowers to a compute kernel whose per-CP output is a
        /// buffer store (see `tryEmitReturnDecl`), so the MSL entry returns
        /// `void`, not the OmegaSL hull-output struct.
        if (inHullShader) {
            out << "void";
        } else {
            writeTypeName(cg.typeResolver->resolveTypeWithExpr(_decl->returnType),
                          _decl->returnType->pointer, out);
        }
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

        /// §5 — the mesh-pipeline payload. MSL is the only backend where the
        /// payload is a genuine function PARAMETER on both stages, passed by
        /// reference in the `object_data` address space:
        ///
        ///   object — `object_data T& p [[payload]]`, writable. Paired with a
        ///     `mesh_grid_properties` handle, which is how MSL expresses the
        ///     child dispatch: `dispatchMesh(x,y,z,p)` lowers to
        ///     `__omegasl_mesh_grid.set_threadgroups_per_grid(uint3(x,y,z))` and
        ///     the payload is not named at the call site at all (it is already
        ///     bound by reference — the write already happened).
        ///
        ///   mesh — `const object_data T& p [[payload]]`, read-only. Emitted
        ///     AFTER the mesh output handle, which must stay first.
        ///
        /// Both are injected here rather than in the generic param loop below
        /// (which suppresses them) so `paramIndex` is bumped before
        /// `emitResourcesAndFillLayout` runs — that is what makes the shared
        /// resource emitter prepend its leading comma correctly.
        payloadParamName.clear();
        for (auto &p : _decl->params) {
            if (p.payload == ast::AttributedFieldDecl::NotPayload) continue;
            payloadParamName = p.name;
            auto sit = structDeclMap.find(std::string(p.typeExpr->name));
            if (sit != structDeclMap.end()) {
                shadermap_entry.payloadDesc.size = ast::payloadStructSize(sit->second);
            }
            if (_decl->shaderType == ast::ShaderDecl::Amplification) {
                out << "object_data " << p.typeExpr->name << "& " << p.name << " [[payload]]";
                ++paramIndex;
                out << ", mesh_grid_properties " << MSL_MESH_GRID_HANDLE;
                ++paramIndex;
                meshHandleEmitted = true;
            } else if (_decl->shaderType == ast::ShaderDecl::Mesh) {
                /// The mesh output handle was emitted immediately above, so this
                /// always needs a separator.
                out << ", const object_data " << p.typeExpr->name << "& " << p.name << " [[payload]]";
                ++paramIndex;
            }
            break;
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
        /// §16 per-patch dispatch — a hull's `VertexID` param is NOT a kernel
        /// parameter: it is the global control-point index, derived per-patch
        /// from `__osl_patch_id` inside a control-point loop (see
        /// `emitShaderEntryBody`). Exclude it from the visible-param count so
        /// the leading comma after the resource block is correct, and skip it
        /// in the emit loop below.
        auto isHullVid = [&](const ast::AttributedFieldDecl &p) {
            return inHullShader && p.attributeName.has_value()
                   && p.attributeName.value() == ATTRIBUTE_VERTEX_ID;
        };
        /// §5 — the payload param is injected above (in the `object_data`
        /// address space), so like the mesh-output params it must NOT be counted
        /// as a visible param here or the leading comma after the resource block
        /// would be emitted for a param that never gets written.
        unsigned visibleParams = 0;
        for (auto &p : _decl->params) {
            if (p.meshOutput == ast::AttributedFieldDecl::NotMeshOutput
                && p.payload == ast::AttributedFieldDecl::NotPayload
                && !isHullVid(p)) ++visibleParams;
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
            /// §5 — the payload param was already injected above, in the
            /// `object_data` address space with its `[[payload]]` attribute.
            if (p.payload != ast::AttributedFieldDecl::NotPayload) {
                continue;
            }
            /// §16 — hull VertexID becomes a per-patch loop local, not a param.
            if (isHullVid(p)) {
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
                const auto &an = p.attributeName.value();
                /// §16 — a `domain`'s `DomainLocation` lowers to
                /// `[[position_in_patch]]`, handled before the generic
                /// `writeAttribute` path (which would emit nothing). (A hull's
                /// `VertexID` never reaches here — it is skipped above and
                /// re-materialized as a per-patch control-point loop local.)
                if (_decl->shaderType == ast::ShaderDecl::Domain && an == ATTRIBUTE_DOMAIN_LOCATION) {
                    out << "[[position_in_patch]]";
                } else {
                    if (an == ATTRIBUTE_VERTEX_ID) {
                        shadermap_entry.vertexShaderInputDesc.useVertexID = true;
                    } else if (an == ATTRIBUTE_GLOBALTHREAD_ID) {
                        shadermap_entry.computeShaderParamsDesc.useGlobalThreadID = true;
                    } else if (an == ATTRIBUTE_THREADGROUP_ID) {
                        shadermap_entry.computeShaderParamsDesc.useThreadGroupID = true;
                    }
                    out << "[[";
                    writeAttribute(an, p.attributeIndex, out);
                    out << "]]";
                }
            }
        }

        /// §16 per-patch dispatch — the hull kernel runs one thread per patch.
        /// Append (a) the patch-id grid param `uint __osl_patch_id
        /// [[thread_position_in_grid]]` (the global control-point index the body
        /// consumes is derived from it in a loop, see `emitShaderEntryBody`) and
        /// (b) the synthesized tessellation-factor buffer `device
        /// MTL{Triangle,Quad}TessellationFactorsHalf* __osl_tess_factors
        /// [[buffer(N)]]` at the next free buffer slot (`bufferCount`, advanced
        /// by the resource loop). A hull always has ≥2 buffer resources, so a
        /// leading comma is always correct here.
        if (inHullShader) {
            out << ", uint __osl_patch_id [[thread_position_in_grid]]";
            out << ", device "
                << (hullDomainIsTri ? "MTLTriangleTessellationFactorsHalf" : "MTLQuadTessellationFactorsHalf")
                << "* __osl_tess_factors [[buffer(" << bufferCount << ")]]";
            ++bufferCount;
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
        ///
        /// §16 per-patch dispatch — for a hull, the kernel runs once per patch
        /// (`__osl_patch_id`). Wrap the per-control-point body in a loop over
        /// the patch's control points, declaring the hull's VertexID local
        /// (global CP index = `patchId * N + cp`) so `controlPoints[vid]` and
        /// the `return`→`hullOut[vid] = …` store (see `tryEmitReturnDecl`)
        /// address the right control point.
        if (inHullShader) {
            auto indent = [&](unsigned lvl){ for (unsigned i = 0; i < lvl; i++) out << "    "; };
            indent(cg.indentLevel);
            out << "for (uint __osl_cp = 0u; __osl_cp < " << hullControlPoints
                << "u; ++__osl_cp) {" << std::endl;
            cg.indentLevel += 1;
            indent(cg.indentLevel);
            out << "uint " << hullVidName << " = __osl_patch_id * " << hullControlPoints
                << "u + __osl_cp;" << std::endl;
            for (auto stmt : _decl->block->body) {
                cg.emitStatementLine(stmt);
            }
            cg.indentLevel -= 1;
            indent(cg.indentLevel);
            out << "}" << std::endl;
        } else {
            for (auto stmt : _decl->block->body) {
                cg.emitStatementLine(stmt);
            }
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

        /// §16 per-patch tess-factor write. The kernel runs once per patch
        /// (thread = `__osl_patch_id`), so after the control-point loop it runs
        /// the patchfn once and writes this patch's factors into the
        /// synthesized factor buffer — unconditionally, no `vid % N == 0` guard
        /// (that was the interim per-CP shape). The MSL factor structs are:
        ///   tri  — `half edgeTessellationFactor[3]; half insideTessellationFactor;`
        ///   quad — `half edgeTessellationFactor[4]; half insideTessellationFactor[2];`
        /// Edge factors are always an array field in OmegaSL (Sema requires
        /// 3/4 edge slots); the tri inside is one slot (scalar or `[0]`).
        if (inHullShader) {
            auto indent = [&](unsigned lvl){ for (unsigned i = 0; i < lvl; i++) out << "    "; };
            unsigned edgeCount = hullDomainIsTri ? 3u : 4u;
            indent(cg.indentLevel);
            out << "auto __pc = " << cg.spellUserFuncName(hullPatchFnName) << "();" << std::endl;
            for (unsigned i = 0; i < edgeCount; i++) {
                indent(cg.indentLevel);
                out << "__osl_tess_factors[__osl_patch_id].edgeTessellationFactor[" << i << "] = half(__pc."
                    << hullEdgeFieldName << (hullEdgeIsArray ? ("[" + std::to_string(i) + "]") : "") << ");" << std::endl;
            }
            if (hullDomainIsTri) {
                indent(cg.indentLevel);
                out << "__osl_tess_factors[__osl_patch_id].insideTessellationFactor = half(__pc."
                    << hullInsideFieldName << (hullInsideIsArray ? "[0]" : "") << ");" << std::endl;
            } else {
                for (unsigned j = 0; j < 2u; j++) {
                    indent(cg.indentLevel);
                    out << "__osl_tess_factors[__osl_patch_id].insideTessellationFactor[" << j << "] = half(__pc."
                        << hullInsideFieldName << (hullInsideIsArray ? ("[" + std::to_string(j) + "]") : "") << ");" << std::endl;
                }
            }
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
        /// §5 — the payload rides BOTH mesh-pipeline stages, so its reset is
        /// unconditional rather than folded into the mesh-only block above.
        payloadParamName.clear();
        /// §16 — reset hull state so the next entry starts clean.
        inHullShader = false;
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
        /// §5.5 Phase C — normalized pack / unpack. MSL has all eight
        /// native but with the longer `pack_float_to_*` / `unpack_*_to_float`
        /// spelling. The signature shape is identical (T → uint / uint → T),
        /// so a plain rename is sufficient.
        if (name == BUILTIN_PACK_SNORM_4X8)    return "pack_float_to_snorm4x8";
        if (name == BUILTIN_UNPACK_SNORM_4X8)  return "unpack_snorm4x8_to_float";
        if (name == BUILTIN_PACK_UNORM_4X8)    return "pack_float_to_unorm4x8";
        if (name == BUILTIN_UNPACK_UNORM_4X8)  return "unpack_unorm4x8_to_float";
        if (name == BUILTIN_PACK_SNORM_2X16)   return "pack_float_to_snorm2x16";
        if (name == BUILTIN_UNPACK_SNORM_2X16) return "unpack_snorm2x16_to_float";
        if (name == BUILTIN_PACK_UNORM_2X16)   return "pack_float_to_unorm2x16";
        if (name == BUILTIN_UNPACK_UNORM_2X16) return "unpack_unorm2x16_to_float";
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
    /// Sub-phase 1.5 — the low-level `ray_query_*` traversal family (MSL,
    /// `intersection_query` from <metal_raytracing>). Returns true (and emits)
    /// when `name` is one of the family; false otherwise. `ray_query_init` is
    /// statement-shaped (build a `ray` first, via statement injection); every
    /// other member is a one-line method call on the RayQuery arg0.
    static bool mslEmitRayQuery(CodeGen &cg, ast::CallExpr *_expr,
                                OmegaCommon::StrRef name, std::ostream &out) {
        if (name == BUILTIN_RAY_QUERY_INIT) {
            std::string q    = cg.renderExprToString(_expr->args[0]);
            std::string as   = cg.renderExprToString(_expr->args[1]);
            std::string ray  = cg.renderExprToString(_expr->args[2]);
            std::string mask = (_expr->args.size() == 4)
                ? ("uint32_t(" + cg.renderExprToString(_expr->args[3]) + ")")
                : std::string("0xFFu");
            unsigned id = cg.rayQueryTempId++;
            std::string r = "_r" + std::to_string(id);
            /// Fully qualify `ray` (see emitIntersect) so a user `Ray` local
            /// named `ray` cannot shadow the Metal type.
            cg.queuePendingStatement("metal::raytracing::ray " + r + "(" + ray + ".origin, "
                + ray + ".direction, " + ray + ".tmin, " + ray + ".tmax);");
            out << q << ".reset(" << r << ", " << as << ", " << mask << ")";
            return true;
        }
        if (name == BUILTIN_RAY_QUERY_COMMITTED) {
            out << "(";
            cg.generateExpr(_expr->args[0]);
            out << ".get_committed_intersection_type() != intersection_type::none)";
            return true;
        }
        const char *method = nullptr;
        if (name == BUILTIN_RAY_QUERY_PROCEED)                 method = "next";
        else if (name == BUILTIN_RAY_QUERY_COMMIT)             method = "commit_triangle_intersection";
        else if (name == BUILTIN_RAY_QUERY_T)                  method = "get_committed_distance";
        else if (name == BUILTIN_RAY_QUERY_PRIMITIVE)          method = "get_committed_primitive_id";
        else if (name == BUILTIN_RAY_QUERY_INSTANCE)           method = "get_committed_instance_id";
        else if (name == BUILTIN_RAY_QUERY_BARYCENTRICS)       method = "get_committed_triangle_barycentric_coord";
        else if (name == BUILTIN_RAY_QUERY_CANDIDATE_T)        method = "get_candidate_triangle_distance";
        else if (name == BUILTIN_RAY_QUERY_CANDIDATE_PRIMITIVE)method = "get_candidate_primitive_id";
        else if (name == BUILTIN_RAY_QUERY_CANDIDATE_INSTANCE) method = "get_candidate_instance_id";
        else if (name == BUILTIN_RAY_QUERY_CANDIDATE_BARYCENTRICS) method = "get_candidate_triangle_barycentric_coord";
        else return false;
        cg.generateExpr(_expr->args[0]);
        out << "." << method << "()";
        return true;
    }

    bool MSLTarget::tryEmitBuiltinCall(CodeGen &cg,
                                       ast::CallExpr *_expr,
                                       OmegaCommon::StrRef name,
                                       std::ostream &out) {
        /// Sub-phase 1.5 — low-level ray-query traversal family.
        if (mslEmitRayQuery(cg, _expr, name, out)) return true;
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
        /// §5 — amplification child dispatch. MSL expresses it as a method on the
        /// `mesh_grid_properties` handle rather than a free call, and the payload
        /// argument is DROPPED: on Metal the payload is already bound to the
        /// object function by reference (`object_data T& [[payload]]`), so by the
        /// time this call runs the writes have landed and there is nothing to
        /// pass. Sema has already proven the 4th argument names exactly that
        /// payload parameter, so dropping it here loses nothing.
        ///
        /// Same shape of divergence as `setMeshOutputs` above — see
        /// Mesh-Shader-Implementation-Plan.md → Phase 5 for the three-way table.
        if (name == BUILTIN_DISPATCH_MESH) {
            if (_expr->args.size() != 4) return false;
            out << MSL_MESH_GRID_HANDLE << ".set_threadgroups_per_grid(uint3(";
            for (unsigned i = 0; i < 3; i++) {
                if (i > 0) out << ", ";
                cg.generateExpr(_expr->args[i]);
            }
            out << "))";
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
        /// §5.5 Phase B — half-float pack / unpack. MSL has no native
        /// `f16tof32` / `f32tof16` / `packHalf2x16` / `unpackHalf2x16`
        /// intrinsic. Lower each through MSL's `as_type<>` template at the
        /// matching half-typed value (the half spelling is `half` /
        /// `half2`; the 16-bit integer spelling is `ushort`):
        ///   f16tof32(uint x)       → float(as_type<half>(ushort(x)))
        ///   f32tof16(float x)      → uint(as_type<ushort>(half(x)))
        ///   packHalf2x16(float2 v) → as_type<uint>(half2(v))
        ///   unpackHalf2x16(uint u) → float2(as_type<half2>(u))
        /// Inline emission (no statement injection) — every form is a
        /// single sub-expression.
        if (name == BUILTIN_F16TOF32) {
            if (_expr->args.size() != 1) return false;
            out << "float(as_type<half>(ushort(";
            cg.generateExpr(_expr->args[0]);
            out << ")))";
            return true;
        }
        if (name == BUILTIN_F32TOF16) {
            if (_expr->args.size() != 1) return false;
            out << "uint(as_type<ushort>(half(";
            cg.generateExpr(_expr->args[0]);
            out << ")))";
            return true;
        }
        if (name == BUILTIN_PACK_HALF_2X16) {
            if (_expr->args.size() != 1) return false;
            out << "as_type<uint>(half2(";
            cg.generateExpr(_expr->args[0]);
            out << "))";
            return true;
        }
        if (name == BUILTIN_UNPACK_HALF_2X16) {
            if (_expr->args.size() != 1) return false;
            out << "float2(as_type<half2>(";
            cg.generateExpr(_expr->args[0]);
            out << "))";
            return true;
        }
        /// §5.5 Phase A — bit-pattern reinterpret. MSL spells all six
        /// directions with a single template: `as_type<TargetN>(operand)`.
        /// The target type is fixed by the builtin name (intN / uintN /
        /// floatN); the arity matches the operand. Sema already validated
        /// the operand is a 32-bit numeric scalar/vector and stamped its
        /// resolved type. No special handling for identity (e.g.
        /// `asint(intN)`) — `as_type<intN>(intN_v)` is a no-op in MSL.
        if (name == BUILTIN_ASINT || name == BUILTIN_ASUINT || name == BUILTIN_ASFLOAT) {
            if (_expr->args.size() != 1) return false;
            using namespace ast::builtins;
            auto *ty = cg.typeResolver->resolveTypeWithExpr(_expr->args[0]->resolvedType);
            int arity = 1;
            if (ty == float2_type || ty == int2_type || ty == uint2_type) arity = 2;
            else if (ty == float3_type || ty == int3_type || ty == uint3_type) arity = 3;
            else if (ty == float4_type || ty == int4_type || ty == uint4_type) arity = 4;
            const char *targetScalar = (name == BUILTIN_ASINT)  ? "int"
                                     : (name == BUILTIN_ASUINT) ? "uint"
                                                                : "float";
            out << "as_type<" << targetScalar;
            if (arity > 1) out << arity;
            out << ">(";
            cg.generateExpr(_expr->args[0]);
            out << ")";
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
        /// §5.6 — atomic operations. Metal carries atomicity on the type, so
        /// every access is an `atomic_*_explicit` call taking `&mem` and
        /// (always) `memory_order_relaxed`. The fetch-ops / exchange / load
        /// return the original value directly — no statement injection. The
        /// operand is cast to the underlying type so the right overload binds.
        {
            const char *verb = nullptr;
            if (name == BUILTIN_ATOMIC_ADD)           verb = "atomic_fetch_add_explicit";
            else if (name == BUILTIN_ATOMIC_MIN)      verb = "atomic_fetch_min_explicit";
            else if (name == BUILTIN_ATOMIC_MAX)      verb = "atomic_fetch_max_explicit";
            else if (name == BUILTIN_ATOMIC_AND)      verb = "atomic_fetch_and_explicit";
            else if (name == BUILTIN_ATOMIC_OR)       verb = "atomic_fetch_or_explicit";
            else if (name == BUILTIN_ATOMIC_XOR)      verb = "atomic_fetch_xor_explicit";
            else if (name == BUILTIN_ATOMIC_EXCHANGE) verb = "atomic_exchange_explicit";
            /// Only an atomic builtin resolves arg0's type — every other
            /// builtin falls through to `renameBuiltin` below. Resolving
            /// unconditionally would deref a null `resolvedType` on builtins
            /// Sema doesn't stamp (e.g. `countbits` reaches here on MSL).
            if (verb || name == BUILTIN_ATOMIC_LOAD || name == BUILTIN_ATOMIC_STORE
                     || name == BUILTIN_ATOMIC_COMPARE_EXCHANGE
                     || name == BUILTIN_ATOMIC_COMPARE_EXCHANGE_WEAK) {
                auto *mty = cg.typeResolver->resolveTypeWithExpr(_expr->args[0]->resolvedType);
                const char *uty = (mty == ast::builtins::atomic_int_type) ? "int" : "uint";
                if (verb) {
                    out << verb << "(&";
                    cg.generateExpr(_expr->args[0]);
                    out << ", (" << uty << ")(";
                    cg.generateExpr(_expr->args[1]);
                    out << "), memory_order_relaxed)";
                    return true;
                }
                if (name == BUILTIN_ATOMIC_LOAD) {
                    out << "atomic_load_explicit(&";
                    cg.generateExpr(_expr->args[0]);
                    out << ", memory_order_relaxed)";
                    return true;
                }
                if (name == BUILTIN_ATOMIC_STORE) {
                    out << "atomic_store_explicit(&";
                    cg.generateExpr(_expr->args[0]);
                    out << ", (" << uty << ")(";
                    cg.generateExpr(_expr->args[1]);
                    out << "), memory_order_relaxed)";
                    return true;
                }
                /// §5.6 Phase B — weak CAS is Metal's *native* primitive, so it
                /// emits inline (bool result + in-place `expected` update via the
                /// `&expected` pointer). `expected` is a thread-local (Sema
                /// rejects a device buffer field, which couldn't form a
                /// `thread T*`). No loop, no statement injection.
                if (name == BUILTIN_ATOMIC_COMPARE_EXCHANGE_WEAK) {
                    out << "atomic_compare_exchange_weak_explicit(&";
                    cg.generateExpr(_expr->args[0]);
                    out << ", &";
                    cg.generateExpr(_expr->args[1]);
                    out << ", (" << uty << ")(";
                    cg.generateExpr(_expr->args[2]);
                    out << "), memory_order_relaxed, memory_order_relaxed)";
                    return true;
                }
                /// §5.6 Phase B — strong CAS. Metal has only the *weak* form, so a
                /// strong CAS (matching HLSL/GLSL) is emulated with the canonical
                /// weak-in-a-loop: loop only while the failure was spurious
                /// (`_exp` still == the captured compare value). `_exp` ends
                /// holding the original value, which is the expression's result.
                /// compare / desired are captured into single-eval temps so the
                /// loop condition and the retried CAS don't re-evaluate them.
                // BUILTIN_ATOMIC_COMPARE_EXCHANGE
                std::string mem = cg.renderExprToString(_expr->args[0]);
                std::string cmp = cg.renderExprToString(_expr->args[1]);
                std::string des = cg.renderExprToString(_expr->args[2]);
                unsigned id = cg.getDimensionsTempId++;
                std::string c = "_caec" + std::to_string(id);
                std::string d = "_caed" + std::to_string(id);
                std::string e = "_caee" + std::to_string(id);
                cg.queuePendingStatement(std::string(uty) + " " + c + " = (" + uty + ")(" + cmp + ");");
                cg.queuePendingStatement(std::string(uty) + " " + d + " = (" + uty + ")(" + des + ");");
                cg.queuePendingStatement(std::string(uty) + " " + e + " = " + c + ";");
                cg.queuePendingStatement("while(!atomic_compare_exchange_weak_explicit(&" + mem
                    + ", &" + e + ", " + d + ", memory_order_relaxed, memory_order_relaxed) && "
                    + e + " == " + c + "){ }");
                out << e;
                return true;
            }
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

    void MSLTarget::emitIntersect(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// Inline ray tracing (Raytracing plan §2.3). `intersect(as, ray[,
        /// mask])` → a Metal `intersector<triangle_data, instancing>`: build a
        /// `ray`, intersect against the `acceleration_structure`, and copy the
        /// result into a `RayHit`. Statement-shaped, so queued as preceding
        /// statements and the expression is the injected `RayHit` temp (same
        /// pattern as the HLSL/GLSL backends). `metal::raytracing` is in scope
        /// via emitDefaultHeaders.
        std::string asStr = cg.renderExprToString(_expr->args[0]);
        std::string rayStr = cg.renderExprToString(_expr->args[1]);
        /// Missing mask ⇒ 0xFF. Metal's `intersect(...)` mask is `uint32_t`; a
        /// literal like `0xFF` resolves to `int` in Sema, so cast an explicit
        /// mask to uint32_t.
        std::string maskStr = (_expr->args.size() == 3)
            ? ("uint32_t(" + cg.renderExprToString(_expr->args[2]) + ")")
            : std::string("0xFFu");

        unsigned id = cg.rayQueryTempId++;
        std::string r = "_r" + std::to_string(id);
        std::string isect = "_isect" + std::to_string(id);
        std::string res = "_res" + std::to_string(id);
        std::string h = "_rh" + std::to_string(id);
        std::string tag = "triangle_data, instancing";

        /// Fully qualify the `ray` type: Metal's `metal::raytracing::ray` is
        /// hidden by ordinary lookup if the user's `Ray` local happens to be
        /// named `ray` (the natural spelling), which would make a bare `ray
        /// _rN(...)` declaration ill-formed. The qualified name is immune.
        cg.queuePendingStatement("metal::raytracing::ray " + r + "(" + rayStr + ".origin, "
            + rayStr + ".direction, " + rayStr + ".tmin, " + rayStr + ".tmax);");
        cg.queuePendingStatement("intersector<" + tag + "> " + isect + ";");
        cg.queuePendingStatement("intersection_result<" + tag + "> " + res + " = "
            + isect + ".intersect(" + r + ", " + asStr + ", " + maskStr + ");");
        cg.queuePendingStatement("RayHit " + h + ";");
        cg.queuePendingStatement(h + ".committed = (" + res + ".type != intersection_type::none);");
        cg.queuePendingStatement(h + ".t = " + res + ".distance;");
        cg.queuePendingStatement(h + ".primitiveIndex = " + res + ".primitive_id;");
        cg.queuePendingStatement(h + ".instanceIndex = " + res + ".instance_id;");
        cg.queuePendingStatement(h + ".barycentrics = " + res + ".triangle_barycentric_coord;");

        out << h;
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
        /// §5.6 — atomic scalars. Metal carries atomicity on the *type*
        /// (`metal::atomic_int`/`atomic_uint`, from `<metal_atomic>` via
        /// `<metal_stdlib>`); every access goes through `atomic_*_explicit`.
        else if(_t == builtins::atomic_int_type){ out << "atomic_int"; }
        else if(_t == builtins::atomic_uint_type){ out << "atomic_uint"; }
        /// Inline ray tracing (Raytracing plan §2.3). Spelled when the TLAS
        /// handle is passed to a user helper; the resource declaration itself
        /// is emitted in `emitResourceBinding`. `metal::raytracing` is in scope
        /// via emitDefaultHeaders. `Ray`/`RayHit` fall through to the default
        /// (`_t->name`) — their names match the emitted structs.
        else if(_t == builtins::acceleration_structure_type){ out << "acceleration_structure<instancing>"; }
        /// Sub-phase 1.5 — the low-level ray-query object. Declared as a local
        /// (`intersection_query<triangle_data, instancing> q;`) and mutated by
        /// the `ray_query_*` intrinsics. `metal::raytracing` is in scope via
        /// emitDefaultHeaders.
        else if(_t == builtins::ray_query_type){ out << "intersection_query<triangle_data, instancing>"; }
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
        /// §4.3 — `double`/`doubleN` have NO MSL mapping on purpose: Metal has
        /// no double-precision type. A shader using `double` must
        /// `#requires(DOUBLE)`, which marks DOUBLE unsatisfied on MSL and stubs
        /// the whole shader before body emission (CodeGen::SHADER_DECL) — so
        /// this writer never reaches a double_type under correct use. (An
        /// undeclared use trips the FeatureScanner warning and then falls to
        /// the `_t->name` default below, which Metal rejects loudly.)
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
