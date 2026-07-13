#include "Target.h"
#include "AST.h"
#include "CodeGen.h"
#include <omegasl.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ostream>
#include <sstream>
#include <unordered_set>
#include <string>
#include <omega-common/multithread.h>

namespace omegasl {

    GLSLTarget::GLSLTarget(GLSLCodeOpts &opts) : Target(Target::GLSL), opts(opts) {
#ifdef TARGET_VULKAN
        compiler = shaderc_compiler_initialize();
#endif
    }
    GLSLTarget::~GLSLTarget() {
#ifdef TARGET_VULKAN
        shaderc_compiler_release(compiler);
#endif
    }

    const char *GLSLTarget::shaderFileExt(ast::ShaderDecl::Type stage) const {
        switch (stage) {
            case ast::ShaderDecl::Vertex:   return ".vert";
            case ast::ShaderDecl::Fragment: return ".frag";
            case ast::ShaderDecl::Compute:  return ".comp";
            case ast::ShaderDecl::Hull:     return ".tesc";
            case ast::ShaderDecl::Domain:   return ".tese";
            case ast::ShaderDecl::Mesh:     return ".mesh";
            /// §5 — Vulkan calls the amplification stage the "task" stage; the
            /// extension's own spelling is what glslc's `-fshader-stage` wants,
            /// and the shared derivation in `compileShader` strips the dot to
            /// get it. (OmegaSL says `amplification` at the source level — see
            /// the plan's Terminology note — but the toolchain flag has to speak
            /// the backend's name.)
            case ast::ShaderDecl::Amplification: return ".task";
        }
        return "";
    }

    bool GLSLTarget::supportsStage(ast::ShaderDecl::Type /*stage*/,
                                   std::string &/*diagnosticOut*/) const {
        /// §2a — GLSL is the first backend to emit mesh source
        /// (`GL_EXT_mesh_shader`); flip every stage on. The other
        /// targets inherit the base `Target::supportsStage` so they
        /// still bail with the precise "mesh codegen not yet
        /// implemented" diagnostic until their own Phase 2 lands.
        return true;
    }

    bool GLSLTarget::compileShader(ast::ShaderDecl::Type stage,
                                   OmegaCommon::StrRef name,
                                   uint64_t requiredFeatures,
                                   const OmegaCommon::FS::Path &srcDir,
                                   const OmegaCommon::FS::Path &outDir) {
        /// GLSL gates 16-bit / 64-bit / etc. via `#extension` directives
        /// the source already carries (emitted by `emitDefaultHeaders`),
        /// so the requiredFeatures bitfield is not consulted here.
        /// glslc takes a stage tag without the dot — derive it from
        /// `shaderFileExt(stage)` so the source-of-truth stays single.
        const char *ext = shaderFileExt(stage);
        const char *shader_stage = (ext[0] == '.') ? ext + 1 : ext;

        auto spvPath = OmegaCommon::FS::Path(outDir).append(name).concat(".spv").absPath();
        auto srcPath = OmegaCommon::FS::Path(srcDir).append(name).concat(ext).absPath();

        std::ostringstream out;
        out << " -fshader-stage=" << shader_stage << " -o " << spvPath << " -c " << srcPath;
        /// §2a — mesh shaders use `GL_EXT_mesh_shader`, which needs the
        /// `SPV_EXT_mesh_shader` capability and therefore SPIR-V 1.4+;
        /// glslc's default SPIR-V 1.0 target rejects the `#extension`
        /// outright. Pinning the target to Vulkan 1.2 (SPIR-V 1.4) is
        /// the minimum that lights up the extension and leaves room for
        /// every other mesh-specific builtin / decoration to lower.
        /// Inline ray tracing (Raytracing plan §2.2) — `GL_EXT_ray_query`
        /// lowers to `SPV_KHR_ray_query`, which (like mesh shaders) needs
        /// SPIR-V 1.4 / Vulkan 1.2; glslc's default SPIR-V 1.0 target rejects
        /// the `#extension` outright. Pin the same vulkan1.2 target when the
        /// shader `#requires(RAYTRACING)`.
        const bool needsRayQuery = (requiredFeatures & OMEGASL_FEATURE_BIT_RAYTRACING) != 0;
        if (stage == ast::ShaderDecl::Mesh || stage == ast::ShaderDecl::Amplification
            || needsRayQuery) {
            out << " --target-env=vulkan1.2";
        }

        auto glslc_process = OmegaCommon::ChildProcess::OpenWithStdoutPipe(opts.glslc_cmd, out.str().c_str());
        auto rc = glslc_process.wait();

        if (rc != 0) {
            std::cerr << "error: glslc failed (exit " << rc << ") for shader '" << name.data() << "'" << std::endl;
            return false;
        }
        return true;
    }

    void GLSLTarget::compileShaderRuntime(ast::ShaderDecl::Type stage,
                                          OmegaCommon::StrRef name,
                                          uint64_t /*requiredFeatures*/,
                                          const std::string &source,
                                          omegasl_shader &meta) {
#ifdef TARGET_VULKAN
        std::string shaderName{name.data(), name.size()};
        std::string dumpPath = "/tmp/OmegaSL-" + shaderName + ".glsl";
        {
            std::ofstream dump(dumpPath, std::ios::out | std::ios::trunc);
            if (dump.is_open()) {
                dump << source;
                dump.close();
                std::cout << "OMEGASL GLSL debug dump: `" << dumpPath << "`" << std::endl;
            }
        }
        auto dumpSourceOnError = [&]() {
            std::ofstream dump(dumpPath, std::ios::out | std::ios::trunc);
            if (dump.is_open()) {
                dump << source;
                dump.close();
                std::cout << "OMEGASL GLSL dump: `" << dumpPath << "`" << std::endl;
            }
        };

        shaderc_shader_kind shader_kind = shaderc_glsl_compute_shader;
        switch (stage) {
            case ast::ShaderDecl::Vertex:   shader_kind = shaderc_glsl_vertex_shader; break;
            case ast::ShaderDecl::Fragment: shader_kind = shaderc_glsl_fragment_shader; break;
            case ast::ShaderDecl::Compute:  shader_kind = shaderc_glsl_compute_shader; break;
            case ast::ShaderDecl::Mesh:     shader_kind = shaderc_glsl_mesh_shader;    break;
            /// §5 — the amplification stage is Vulkan's "task" stage.
            case ast::ShaderDecl::Amplification: shader_kind = shaderc_glsl_task_shader; break;
            /// §16 Phase G — a hull compiles as a `.tesc` (tessellation control)
            /// and a domain as a `.tese` (tessellation evaluation). Without the
            /// right kind shaderc treats the source as compute and produces
            /// invalid bytecode (the loader then rejects the module).
            case ast::ShaderDecl::Hull:     shader_kind = shaderc_glsl_tess_control_shader;    break;
            case ast::ShaderDecl::Domain:   shader_kind = shaderc_glsl_tess_evaluation_shader; break;
        }

        auto options = shaderc_compile_options_initialize();
        /// §2a — runtime mirror of the offline glslc gate: mesh shaders
        /// need at least Vulkan 1.2 / SPIR-V 1.4 for the
        /// `SPV_EXT_mesh_shader` capability. shaderc defaults to
        /// SPIR-V 1.0 and rejects the `#extension` otherwise.
        if (stage == ast::ShaderDecl::Mesh || stage == ast::ShaderDecl::Amplification) {
            shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan,
                                                   shaderc_env_version_vulkan_1_2);
            shaderc_compile_options_set_target_spirv(options, shaderc_spirv_version_1_4);
        }

        meta.data = nullptr;
        meta.dataSize = 0;

        auto result = shaderc_compile_into_spv(compiler, source.data(), source.size(), shader_kind,
                                               name.data(), "main", options);

        if (result == nullptr) {
            std::cout << "OMEGASL COMPILE ERROR: shaderc returned null result for `" << name.data() << "`." << std::endl;
            dumpSourceOnError();
            shaderc_compile_options_release(options);
            return;
        }

        auto status = shaderc_result_get_compilation_status(result);
        if (status != shaderc_compilation_status_success) {
            std::cout << "OMEGASL COMPILE ERROR (" << status << ") in `" << name.data() << "`: "
                      << shaderc_result_get_error_message(result) << std::endl;
            dumpSourceOnError();
            shaderc_result_release(result);
            shaderc_compile_options_release(options);
            return;
        }

        auto byteLen = shaderc_result_get_length(result);
        if (byteLen == 0) {
            std::cout << "OMEGASL COMPILE ERROR: shader `" << name.data() << "` produced empty SPIR-V output." << std::endl;
            dumpSourceOnError();
            shaderc_result_release(result);
            shaderc_compile_options_release(options);
            return;
        }

        auto *spirvBytes = new std::uint8_t[byteLen];
        std::memcpy(spirvBytes, shaderc_result_get_bytes(result), byteLen);
        meta.data = spirvBytes;
        meta.dataSize = byteLen;

        shaderc_compile_options_release(options);
        shaderc_result_release(result);
#else
        (void)stage;
        (void)name;
        (void)source;
        (void)meta;
#endif
    }

    void GLSLTarget::resetForNextShader() {
        binding = 0;
    }

    void GLSLTarget::emitStaticPreamble(std::ostream &/*out*/) {
        /// GLSL emits each binding inline at file scope; no preamble.
    }

    /// Prefix identifiers that collide with GLSL reserved words.
    /// Used both via the public `writeIdentifier` Target hook (called
    /// from the shared `ID_EXPR` arm) and inside this file's resource
    /// binding emission.
    static void writeGLSLIdent(const OmegaCommon::StrRef &name, std::ostream &out) {
        if (name == "input" || name == "output" ||
            name == "shared" || name == "filter" ||
            name == "common" || name == "partition" ||
            name == "active" || name == "resource") {
            out << "_";
        }
        out.write(name.data(), name.size());
    }

    void GLSLTarget::writeIdentifier(OmegaCommon::StrRef name, std::ostream &out) const {
        writeGLSLIdent(name, out);
    }

    const char *GLSLTarget::shaderObjectFileExt(ast::ShaderDecl::Type /*stage*/) const {
        return ".spv";
    }

    /// Inline ray tracing (Raytracing plan §1.2/§2.2). Fixed GLSL text for the
    /// `Ray` / `RayHit` builtin structs. Terminated with `};` and no trailing
    /// newline, matching what `emitStructDecl` caches for a user struct, so the
    /// used-struct emission path spells them identically.
    static const char glslRayStruct[] =
        "struct Ray {\n"
        "  vec3 origin;\n"
        "  vec3 direction;\n"
        "  float tmin;\n"
        "  float tmax;\n"
        "};";
    static const char glslRayHitStruct[] =
        "struct RayHit {\n"
        "  bool committed;\n"
        "  float t;\n"
        "  uint primitiveIndex;\n"
        "  uint instanceIndex;\n"
        "  vec2 barycentrics;\n"
        "};";

    void GLSLTarget::emitDefaultHeaders(CodeGen &cg, std::ostream &out) {
        /// Inline ray tracing (Raytracing plan §2.2) — `GL_EXT_ray_query`
        /// requires GLSL 4.60; everything else compiles fine under 4.50. Bump
        /// the `#version` only when the shader `#requires(RAYTRACING)` so non-RT
        /// output stays byte-identical to the pre-RT emitter.
        const bool needsRaytracing =
            (cg.fileRequiredFeatures & OMEGASL_FEATURE_BIT_RAYTRACING) != 0;
        out << "// Warning! This file has been generated by omegaslc! DO NOT EDIT!!\n"
            << (needsRaytracing ? "#version 460\n" : "#version 450\n")
            << "#extension GL_EXT_samplerless_texture_functions : enable" << std::endl;

        /// §4.1 / §4.2 — GLSL needs explicit `#extension` directives to
        /// expose the 16-bit and 64-bit explicit-arithmetic types. The
        /// gating is the file's `#requires(...)` set: declaring the
        /// requirement at the top of the source flips the bit and the
        /// extension lines come along for the ride. Without the
        /// directives the GLSL frontend rejects `float16_t` /
        /// `int64_t` etc. with a confusing error, so emit them on the
        /// declared-feature side rather than relying on the GLSL
        /// compiler's implicit behavior.
        if(cg.fileRequiredFeatures & OMEGASL_FEATURE_BIT_FLOAT16){
            out << "#extension GL_EXT_shader_explicit_arithmetic_types_float16 : enable\n";
            out << "#extension GL_EXT_shader_explicit_arithmetic_types_int16 : enable" << std::endl;
        }
        if(cg.fileRequiredFeatures & OMEGASL_FEATURE_BIT_INT64){
            out << "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : enable" << std::endl;
        }
        /// §4.3 — double-precision floats need `GL_ARB_gpu_shader_fp64`
        /// (core in GLSL 4.0 / SPIR-V Float64). Emitted on `#requires(DOUBLE)`,
        /// mirroring the FLOAT16/INT64 extension pattern.
        if(cg.fileRequiredFeatures & OMEGASL_FEATURE_BIT_DOUBLE){
            out << "#extension GL_ARB_gpu_shader_fp64 : enable" << std::endl;
        }
        /// Inline ray tracing (Raytracing plan §2.2). `GL_EXT_ray_query`
        /// provides `rayQueryEXT`, `accelerationStructureEXT`, and the
        /// `rayQuery*EXT` traversal builtins. Emitted on `#requires(RAYTRACING)`,
        /// mirroring the FLOAT16/INT64/DOUBLE extension pattern. Also pre-seed
        /// the `Ray`/`RayHit` struct text into `generatedStructs` so the
        /// used-struct emission spells them (they resolve `builtin = false`, so
        /// declaring a `Ray`/`RayHit` local adds their names to the used set).
        if(needsRaytracing){
            out << "#extension GL_EXT_ray_query : require" << std::endl;
            generatedStructs.insert(std::make_pair(std::string("Ray"), std::string(glslRayStruct)));
            generatedStructs.insert(std::make_pair(std::string("RayHit"), std::string(glslRayHitStruct)));
        }
    }

    /// §16 — a patch-constant struct (an `internal` struct carrying
    /// `TessFactor`/`InsideTessFactor` fields) is the value a hull's `patchfn`
    /// returns to drive `gl_TessLevelOuter/Inner[]`. It is *not* an inter-stage
    /// varying, so GLSL must treat it as a plain data struct: emit the `struct`,
    /// don't destructure it into `<struct>_<field>` `out` varyings, and don't
    /// reroute its field references. Detected by the presence of a factor field.
    static bool isPatchConstantStruct(ast::StructDecl *d) {
        if (!d->internal) return false;
        for (auto &f : d->fields) {
            if (f.attributeName.has_value()
                && (f.attributeName.value() == ATTRIBUTE_TESS_FACTOR
                    || f.attributeName.value() == ATTRIBUTE_INSIDE_TESS_FACTOR)) {
                return true;
            }
        }
        return false;
    }

    void GLSLTarget::emitStructDecl(CodeGen &cg, ast::StructDecl *_decl) {
        std::ostringstream out;
        if (_decl->internal && !isPatchConstantStruct(_decl)) {
            internalStructs.push_back(_decl);
        } else {
            out << "struct " << _decl->name << " {" << std::endl;
            for (auto &f : _decl->fields) {
                out << "  ";
                cg.writeTypeExpr(f.typeExpr, out);
                out << " " << f.name;
                cg.writeDeclTypeSuffix(f.typeExpr, out);
                out << ";" << std::endl;
            }
            out << "};";
        }
        generatedStructs.insert(std::make_pair(_decl->name, out.str()));
        structDeclMap[_decl->name] = _decl;
    }

    /// Plain (non-`internal`) struct definitions are emitted here — BEFORE
    /// the user-function prototypes, which may reference them by value.
    /// Interstage-varying emission for `internal` structs stays in
    /// `emitShaderEntryHeader` (it is positional: varyings interleave with
    /// the fragment-output decls).
    void GLSLTarget::emitShaderUsedStructs(CodeGen &cg, ast::ShaderDecl *_decl,
                                           std::ostream &out) {
        std::vector<std::string> used;
        cg.typeResolver->getStructsInFuncDecl(_decl, used);
        for (auto &s : used) {
            auto pred = [&](ast::StructDecl *d) -> bool { return d->name == s; };
            auto it = std::find_if(internalStructs.begin(), internalStructs.end(), pred);
            if (it == internalStructs.end()) {
                out << generatedStructs[s] << std::endl << std::endl;
            }
        }
    }

    /// §6.1 — GLSL `shared` is only valid at global scope, so each top-level
    /// `threadgroup` local in the compute body is hoisted here as
    /// `shared T name[dims];`. The custom entry-body loop skips the
    /// original decl.
    void GLSLTarget::emitThreadgroupGlobals(CodeGen &cg, ast::ShaderDecl *_decl,
                                            std::ostream &out) {
        if (_decl->shaderType != ast::ShaderDecl::Compute || !_decl->block) {
            return;
        }
        for (auto *stmt : _decl->block->body) {
            if (stmt->type != VAR_DECL) continue;
            auto *_var = (ast::VarDecl *)stmt;
            if (!_var->isThreadgroup) continue;
            out << "shared ";
            cg.writeTypeExpr(_var->typeExpr, out);
            out << " ";
            writeIdentifier(_var->spec.name, out);
            cg.writeDeclTypeSuffix(_var->typeExpr, out);
            out << ";" << std::endl;
        }
    }

    bool GLSLTarget::tryEmitVarDecl(CodeGen &cg, ast::VarDecl *_decl) {
        std::ostream &shaderOut = cg.getShaderOut();
        auto pred = [&](ast::StructDecl *d) -> bool {
            return d->name == _decl->typeExpr->name;
        };
        auto internal_struct_found = std::find_if(internalStructs.begin(),
                                                  internalStructs.end(), pred);
        if (internal_struct_found == internalStructs.end()) {
            return false;
        }
        /// Track variable with internal struct.
        internalStructVarMap.push_back(std::make_pair(_decl->spec.name, *internal_struct_found));

        /// Brace-initializer (`Raster r = { pos, uv };`) decomposes into
        /// per-field stores against gl_Position / output varyings. Without
        /// this the initializer would silently drop, leaving gl_Position
        /// unset.
        if (_decl->spec.initializer.has_value() &&
            _decl->spec.initializer.value()->type == ARRAY_EXPR) {
            auto *initList = (ast::ArrayExpr *)_decl->spec.initializer.value();
            auto *structDecl = *internal_struct_found;
            size_t count = std::min(initList->elm.size(), structDecl->fields.size());
            for (size_t fi = 0; fi < count; fi++) {
                for (unsigned i = 0; i < cg.indentLevel; i++) {
                    shaderOut << "  ";
                }
                auto &field = structDecl->fields[fi];
                writeInternalFieldRef(field, structDecl->name, shaderOut);
                shaderOut << " = ";
                cg.generateExpr(initList->elm[fi]);
                shaderOut << ";" << std::endl;
            }
        }
        return true;
    }

    bool GLSLTarget::tryEmitReturnDecl(CodeGen &cg, ast::ReturnDecl *_decl) {
        std::ostream &shaderOut = cg.getShaderOut();
        if (_decl->expr && currentShaderType == ast::ShaderDecl::Fragment
            && fragmentOutputStruct != nullptr) {
            /// Per-field stores already happened — bare `return`. Flush any
            /// synthetic `OutputCoverage` uint local back into the int-typed
            /// `gl_SampleMask[0]` builtin first; see `emitShaderEntryBody`
            /// for the rationale.
            for (auto &f : fragmentOutputStruct->fields) {
                if (f.attributeName.has_value()
                    && f.attributeName.value() == ATTRIBUTE_OUTPUT_COVERAGE) {
                    shaderOut << "gl_SampleMask[0] = int("
                              << fragmentOutputStruct->name << "_" << f.name
                              << ");" << std::endl;
                    for (unsigned i = 0; i < cg.indentLevel; i++) shaderOut << "    ";
                }
            }
            shaderOut << "return";
            return true;
        }
        if (_decl->expr && currentShaderType == ast::ShaderDecl::Fragment
            && !activeReturnReplacement.empty()) {
            shaderOut << activeReturnReplacement << " = ";
            cg.generateExpr(_decl->expr);
            shaderOut << ";" << std::endl;
            for (unsigned i = 0; i < cg.indentLevel; i++) {
                shaderOut << "  ";
            }
            shaderOut << "return";
            return true;
        }
        return false;
    }

    void GLSLTarget::emitShaderEntryHeader(CodeGen &cg,
                                           ast::ShaderDecl *_decl,
                                           omegasl_shader &shader_entry,
                                           std::ostream &out) {
        currentShaderType = _decl->shaderType;

        OmegaCommon::String return_val_replacement;
        switch (_decl->shaderType) {
            case ast::ShaderDecl::Vertex:   return_val_replacement = "gl_Position"; break;
            case ast::ShaderDecl::Fragment: return_val_replacement = "_outColor";   break;
            case ast::ShaderDecl::Compute:  break;
            case ast::ShaderDecl::Hull:     return_val_replacement = "gl_Position"; break;
            case ast::ShaderDecl::Domain:   return_val_replacement = "gl_Position"; break;
            case ast::ShaderDecl::Mesh:     break;
            case ast::ShaderDecl::Amplification: break;
        }
        activeReturnReplacement = return_val_replacement;

        if (_decl->shaderType == ast::ShaderDecl::Compute) {
            shader_entry.threadgroupDesc.x = _decl->threadgroupDesc.x;
            shader_entry.threadgroupDesc.y = _decl->threadgroupDesc.y;
            shader_entry.threadgroupDesc.z = _decl->threadgroupDesc.z;
        }

        /// Tessellation layout directives.
        if (_decl->shaderType == ast::ShaderDecl::Hull) {
            out << "layout(vertices = " << _decl->tessDesc.outputControlPoints << ") out;" << std::endl;
        } else if (_decl->shaderType == ast::ShaderDecl::Domain) {
            auto &td = _decl->tessDesc;
            const char *domStr = (td.domain == ast::ShaderDecl::TessellationDesc::Triangle) ? "triangles" : "quads";
            const char *spacingStr = td.partitioning == ast::ShaderDecl::TessellationDesc::Integer ? "equal_spacing" :
                                     td.partitioning == ast::ShaderDecl::TessellationDesc::FractionalEven ? "fractional_even_spacing" : "fractional_odd_spacing";
            const char *windStr = (td.outputTopology == ast::ShaderDecl::TessellationDesc::TriangleCCW) ? "ccw" : "cw";
            out << "layout(" << domStr << ", " << spacingStr << ", " << windStr << ") in;" << std::endl;
        }

        /// §2a — mesh stage prologue. The `GL_EXT_mesh_shader` directives
        /// must appear before any use of the extension's types/builtins
        /// (which is everything else in this entry's emission). Layout
        /// then mirrors the descriptor: `local_size_*` from the per-meshlet
        /// workgroup (carried in `threadgroupDesc`, exactly like compute)
        /// and `max_vertices` / `max_primitives` / topology from `meshDesc`.
        if (_decl->shaderType == ast::ShaderDecl::Mesh) {
            out << "#extension GL_EXT_mesh_shader : require" << std::endl;
            shader_entry.threadgroupDesc.x = _decl->threadgroupDesc.x;
            shader_entry.threadgroupDesc.y = _decl->threadgroupDesc.y;
            shader_entry.threadgroupDesc.z = _decl->threadgroupDesc.z;
            shader_entry.meshDesc.max_vertices = _decl->meshDesc.maxVertices;
            shader_entry.meshDesc.max_primitives = _decl->meshDesc.maxPrimitives;
            shader_entry.meshDesc.topology = static_cast<int>(_decl->meshDesc.topology);
            meshMaxVertices = _decl->meshDesc.maxVertices;
            meshMaxPrimitives = _decl->meshDesc.maxPrimitives;
            meshTopology = _decl->meshDesc.topology;
            const char *topoStr = (_decl->meshDesc.topology == ast::ShaderDecl::MeshDesc::Triangle) ? "triangles"
                                : (_decl->meshDesc.topology == ast::ShaderDecl::MeshDesc::Line)     ? "lines"
                                                                                                    : "points";
            out << "layout(local_size_x = " << _decl->threadgroupDesc.x
                << ", local_size_y = " << _decl->threadgroupDesc.y
                << ", local_size_z = " << _decl->threadgroupDesc.z << ") in;" << std::endl;
            out << "layout(max_vertices = " << meshMaxVertices
                << ", max_primitives = " << meshMaxPrimitives
                << ", " << topoStr << ") out;" << std::endl;
        }

        /// §5 — task (amplification) stage prologue. Same extension + workgroup
        /// shape as the mesh stage above; a task shader has no `out` layout of
        /// its own because it emits no primitives — it emits a *grid* (via
        /// `EmitMeshTasksEXT`, from the body) plus a payload (declared below).
        if (_decl->shaderType == ast::ShaderDecl::Amplification) {
            out << "#extension GL_EXT_mesh_shader : require" << std::endl;
            shader_entry.threadgroupDesc.x = _decl->threadgroupDesc.x;
            shader_entry.threadgroupDesc.y = _decl->threadgroupDesc.y;
            shader_entry.threadgroupDesc.z = _decl->threadgroupDesc.z;
            out << "layout(local_size_x = " << _decl->threadgroupDesc.x
                << ", local_size_y = " << _decl->threadgroupDesc.y
                << ", local_size_z = " << _decl->threadgroupDesc.z << ") in;" << std::endl;
        }

        shader_entry.type = _decl->shaderType == ast::ShaderDecl::Vertex   ? OMEGASL_SHADER_VERTEX
                          : _decl->shaderType == ast::ShaderDecl::Fragment ? OMEGASL_SHADER_FRAGMENT
                          : _decl->shaderType == ast::ShaderDecl::Compute  ? OMEGASL_SHADER_COMPUTE
                          : _decl->shaderType == ast::ShaderDecl::Hull     ? OMEGASL_SHADER_HULL
                          : _decl->shaderType == ast::ShaderDecl::Domain   ? OMEGASL_SHADER_DOMAIN
                          : _decl->shaderType == ast::ShaderDecl::Mesh     ? OMEGASL_SHADER_MESH
                                                                            : OMEGASL_SHADER_AMPLIFICATION;
        shader_entry.name = new char[_decl->name.size() + 1];
        std::copy(_decl->name.begin(), _decl->name.end(), (char *)shader_entry.name);
        ((char *)shader_entry.name)[_decl->name.size()] = '\0';

        /// Detect fragment-output struct return.
        fragmentOutputStruct = nullptr;
        if (_decl->shaderType == ast::ShaderDecl::Fragment) {
            auto retIt = structDeclMap.find(_decl->returnType->name);
            if (retIt != structDeclMap.end() && retIt->second->internal) {
                fragmentOutputStruct = retIt->second;
            }
        }

        /// §2a — locate the mesh stage's `out vertices` element struct and
        /// `out indices` parameter so the body's `verts[i].field = ...` and
        /// `tris[i] = ...` writes can be rerouted at MEMBER_EXPR /
        /// INDEX_EXPR emission time. Sema has already proven the entry
        /// declares exactly one of each, with the right element types and
        /// extents; we just remember the names + decl here. Reset to the
        /// "not a mesh shader" defaults for every other stage.
        meshVertsParamName.clear();
        meshIndicesParamName.clear();
        meshVertsStructDecl = nullptr;
        controlPointArrayParams.clear();
        if (_decl->shaderType == ast::ShaderDecl::Mesh) {
            for (auto &p : _decl->params) {
                if (p.meshOutput == ast::AttributedFieldDecl::Vertices) {
                    meshVertsParamName = p.name;
                    auto sit = structDeclMap.find(p.typeExpr->name);
                    if (sit != structDeclMap.end()) meshVertsStructDecl = sit->second;
                } else if (p.meshOutput == ast::AttributedFieldDecl::Indices) {
                    meshIndicesParamName = p.name;
                }
            }
        }

        /// §5 — the mesh-pipeline payload. Both stages declare it identically in
        /// GLSL: one file-scope `taskPayloadSharedEXT <Struct> <name>;`, writable
        /// on the task side and read-only on the mesh side (the extension infers
        /// the direction from the stage, so there is nothing to spell out). The
        /// declaration carries the user's own parameter name, which is why the
        /// body needs no rewriting whatsoever — `p.baseTriangle` in the source is
        /// already `p.baseTriangle` against this global. The parameter itself is
        /// suppressed from `void main()` further down (a declaration, not an arg).
        ///
        /// The payload's struct type is a plain (non-`internal`) struct, so
        /// `emitShaderUsedStructs` has already emitted its definition above this
        /// point — the ordering is what makes this decl legal.
        payloadParamName.clear();
        if (_decl->shaderType == ast::ShaderDecl::Mesh
            || _decl->shaderType == ast::ShaderDecl::Amplification) {
            for (auto &p : _decl->params) {
                if (p.payload == ast::AttributedFieldDecl::NotPayload) continue;
                payloadParamName = p.name;
                out << "taskPayloadSharedEXT " << p.typeExpr->name << " " << p.name << ";" << std::endl;
                auto sit = structDeclMap.find(p.typeExpr->name);
                if (sit != structDeclMap.end()) {
                    shader_entry.payloadDesc.size = ast::payloadStructSize(sit->second);
                }
                break;
            }
        }

        /// Emit interstage varying decls for internal structs (input on
        /// fragment, output on others); plain `struct` decls for the rest.
        OmegaCommon::Vector<OmegaCommon::String> all_used_structs;
        cg.typeResolver->getStructsInFuncDecl(_decl, all_used_structs);
        for (auto &s : all_used_structs) {
            auto pred = [&](ast::StructDecl *d) -> bool { return d->name == s; };
            auto it = std::find_if(internalStructs.begin(), internalStructs.end(), pred);
            if (it != internalStructs.end()) {
                auto &_struct = *it;
                /// The fragment output struct is emitted separately below.
                if (_struct == fragmentOutputStruct) continue;
                /// §2a — the mesh vertex output struct is emitted as
                /// per-field arrayed `out` varyings (one `[max_vertices]`
                /// array per non-Position field) further down, since each
                /// vertex slot in the meshlet writes its own copy. Skip
                /// the normal scalar interstage emission for it.
                if (_decl->shaderType == ast::ShaderDecl::Mesh && _struct == meshVertsStructDecl) continue;
                OmegaCommon::String mode =
                    (_decl->shaderType == ast::ShaderDecl::Fragment) ? "in" : "out";
                unsigned idx = 0;
                for (auto &f : _struct->fields) {
                    bool isPosition = f.attributeName.has_value()
                        && f.attributeName.value() == ATTRIBUTE_POSITION;
                    bool isClip = f.attributeName.has_value()
                        && f.attributeName.value() == ATTRIBUTE_CLIP_DISTANCE;
                    bool isCull = f.attributeName.has_value()
                        && f.attributeName.value() == ATTRIBUTE_CULL_DISTANCE;
                    /// A `Position` field rides gl_Position / gl_FragCoord and
                    /// gets no varying. Clip/cull distance ride the
                    /// gl_ClipDistance[] / gl_CullDistance[] builtin arrays,
                    /// redeclared with an explicit size on the producing stage
                    /// (not a fragment input). Every other field — including an
                    /// unattributed one, which Sema permits on internal structs
                    /// — is a location-based varying. This matches
                    /// `writeInternalFieldRef`, which routes each field to the
                    /// same target.
                    if (isPosition) {
                        /// implicit gl_Position; nothing to declare here.
                    } else if (isClip || isCull) {
                        /// §1.7 — emit the builtin-array redeclaration only on
                        /// the producing (vertex/hull/domain) side.
                        if (mode == "out") {
                            unsigned n = f.typeExpr->arrayDims.empty()
                                         ? 1u : f.typeExpr->arrayDims[0];
                            out << "out float "
                                << (isClip ? "gl_ClipDistance" : "gl_CullDistance")
                                << "[" << n << "];" << std::endl;
                        }
                    } else {
                        out << "layout(location =" << idx << ") ";
                        /// §1.6 — interpolation qualifier, between layout and the
                        /// in/out storage qualifier. The same field drives both
                        /// the vertex `out` and fragment `in` varying, so the
                        /// qualifier matches across stages by construction.
                        switch (f.interp) {
                            case ast::AttributedFieldDecl::Flat:          out << "flat "; break;
                            case ast::AttributedFieldDecl::Centroid:      out << "centroid "; break;
                            case ast::AttributedFieldDecl::Sample:        out << "sample "; break;
                            case ast::AttributedFieldDecl::NoPerspective: out << "noperspective "; break;
                            default: break;
                        }
                        out << mode << " ";
                        writeTypeName(cg.typeResolver->resolveTypeWithExpr(f.typeExpr),
                                      f.typeExpr->pointer, out);
                        out << " " << _struct->name << "_" << f.name << ";" << std::endl;
                    }
                    idx += 1;
                }
            }
            /// Plain (non-internal) structs were already emitted by
            /// emitShaderUsedStructs, before the user-function prototypes.
        }

        extra_stmts.str("");
        extra_stmts.clear();

        cg.indentLevel += 1;

        if (_decl->shaderType == ast::ShaderDecl::Fragment) {
            /// §1.5 — early depth/stencil: a global execution-mode directive on
            /// the fragment stage (must be at file scope, before `void main()`).
            if (_decl->earlyDepthStencil) {
                out << "layout(early_fragment_tests) in;" << std::endl;
            }
            if (fragmentOutputStruct) {
                /// MRT / depth output: one `layout(location=N) out vec4` per
                /// `Color(N)` field. `Depth` rides on `gl_FragDepth` (no
                /// decl). `OutputCoverage` rides on `gl_SampleMask[0]` but
                /// the type is `int[]`, not `uint`, so we route writes to
                /// a synthetic `uint <struct>_<field>` local declared at
                /// the top of main(); `emitShaderEntryBody` flushes the
                /// `int(...)` cast back into `gl_SampleMask[0]` before
                /// each return.
                for (auto &f : fragmentOutputStruct->fields) {
                    if (!f.attributeName.has_value()) continue;
                    if (f.attributeName.value() == ATTRIBUTE_COLOR
                        && f.attributeIndex.has_value()) {
                        unsigned loc = f.attributeIndex.value();
                        out << "layout(location=" << loc << ") out vec4 _outColor"
                            << loc << ";" << std::endl;
                    }
                    else if (f.attributeName.value() == ATTRIBUTE_OUTPUT_COVERAGE) {
                        for (unsigned i = 0; i < cg.indentLevel; i++) extra_stmts << "    ";
                        extra_stmts << "uint " << fragmentOutputStruct->name
                                    << "_" << f.name << ";" << std::endl;
                    }
                }
            } else {
                out << "layout(location = 0) out vec4 _outColor";
                out << ";" << std::endl;
            }
        }

        /// §2a — per-vertex arrayed varyings for the mesh stage. Each
        /// non-Position field of the verts element struct becomes a
        /// `layout(location=N) out T <struct>_<field>[max_vertices];`
        /// array; the matching `writeInternalFieldRef` reroutes user
        /// writes (`verts[i].field`) onto `<struct>_<field>[i]`. The
        /// `Position` semantic instead rides `gl_MeshVerticesEXT[i]
        /// .gl_Position` (declared implicitly by the extension), so it
        /// emits no `layout(location=...) out`. Skipping a field's
        /// location entirely would collide the indices with the
        /// downstream fragment-input layout; declaring it anyway keeps
        /// the location numbering identical to a vertex-stage emission
        /// of the same struct, which is what the fragment side expects.
        if (_decl->shaderType == ast::ShaderDecl::Mesh && meshVertsStructDecl) {
            unsigned idx = 0;
            for (auto &f : meshVertsStructDecl->fields) {
                bool isPosition = f.attributeName.has_value()
                    && f.attributeName.value() == ATTRIBUTE_POSITION;
                if (!isPosition) {
                    out << "layout(location=" << idx << ") ";
                    switch (f.interp) {
                        case ast::AttributedFieldDecl::Flat:          out << "flat "; break;
                        case ast::AttributedFieldDecl::Centroid:      out << "centroid "; break;
                        case ast::AttributedFieldDecl::Sample:        out << "sample "; break;
                        case ast::AttributedFieldDecl::NoPerspective: out << "noperspective "; break;
                        default: break;
                    }
                    out << "out ";
                    writeTypeName(cg.typeResolver->resolveTypeWithExpr(f.typeExpr),
                                  f.typeExpr->pointer, out);
                    out << " " << meshVertsStructDecl->name << "_" << f.name
                        << "[" << meshMaxVertices << "];" << std::endl;
                }
                idx += 1;
            }
        }

        /// Resource bindings (file scope for GLSL).
        cg.emitResourcesAndFillLayout(_decl, shader_entry, out);

        /// Standard shader arguments. Compute uses `layout(local_size_*)`
        /// + attribute-bridge locals; vertex/hull/domain use
        /// `layout(location=N) in` + attribute-bridge for VertexID;
        /// mesh suppresses its `out vertices` / `out indices` params (they
        /// route to `gl_MeshVerticesEXT` / `gl_Primitive*IndicesEXT` and
        /// have no representation in `void main()`'s signature) and
        /// bridges thread-IDs from `gl_GlobalInvocationID` etc. just like
        /// compute; fragment scalar inputs bridge from `gl_*` builtins;
        /// fragment struct inputs land in `internalStructVarMap` for
        /// member rerouting at body emission time.
        if (_decl->shaderType == ast::ShaderDecl::Compute) {
            out << "layout(local_size_x = " << _decl->threadgroupDesc.x
                << ", local_size_y = " << _decl->threadgroupDesc.y
                << ", local_size_z = " << _decl->threadgroupDesc.z << ") in;" << std::endl;
            for (unsigned arg_idx = 0; arg_idx < _decl->params.size(); arg_idx++) {
                auto &arg = _decl->params[arg_idx];
                if (arg.attributeName.has_value()) {
                    const char *builtin = nullptr;
                    if (arg.attributeName.value() == ATTRIBUTE_GLOBALTHREAD_ID) {
                        builtin = "gl_GlobalInvocationID";
                        shader_entry.computeShaderParamsDesc.useGlobalThreadID = true;
                    } else if (arg.attributeName.value() == ATTRIBUTE_LOCALTHREAD_ID) {
                        builtin = "gl_LocalInvocationID";
                        shader_entry.computeShaderParamsDesc.useLocalThreadID = true;
                    } else if (arg.attributeName.value() == ATTRIBUTE_THREADGROUP_ID) {
                        builtin = "gl_WorkGroupID";
                        shader_entry.computeShaderParamsDesc.useThreadGroupID = true;
                    }
                    if (builtin != nullptr) {
                        for (unsigned i = 0; i < cg.indentLevel; i++) extra_stmts << "    ";
                        writeTypeName(cg.typeResolver->resolveTypeWithExpr(arg.typeExpr),
                                      arg.typeExpr->pointer, extra_stmts);
                        extra_stmts << " " << arg.name << " = " << builtin << ";" << std::endl;
                    }
                }
            }
        } else if (_decl->shaderType == ast::ShaderDecl::Mesh
                   || _decl->shaderType == ast::ShaderDecl::Amplification) {
            /// §2a / §5 — mesh and amplification both share compute's thread-ID
            /// model (`local_size_*` already emitted above with each stage's
            /// prologue). Three kinds of parameter get suppressed from
            /// `void main()` because GLSL expresses them as declarations rather
            /// than arguments: `out vertices` / `out indices` (addressed via the
            /// `gl_Mesh*EXT` builtins at body emission) and the payload (the
            /// `taskPayloadSharedEXT` global emitted above). Everything else with
            /// an attribute bridges from its `gl_*` builtin into a local of the
            /// user's chosen name, identical to the compute path.
            for (auto &arg : _decl->params) {
                if (arg.meshOutput != ast::AttributedFieldDecl::NotMeshOutput) continue;
                if (arg.payload != ast::AttributedFieldDecl::NotPayload) continue;
                if (arg.attributeName.has_value()) {
                    const char *builtin = nullptr;
                    if (arg.attributeName.value() == ATTRIBUTE_GLOBALTHREAD_ID) {
                        builtin = "gl_GlobalInvocationID";
                    } else if (arg.attributeName.value() == ATTRIBUTE_LOCALTHREAD_ID) {
                        builtin = "gl_LocalInvocationID";
                    } else if (arg.attributeName.value() == ATTRIBUTE_THREADGROUP_ID) {
                        builtin = "gl_WorkGroupID";
                    }
                    if (builtin != nullptr) {
                        for (unsigned i = 0; i < cg.indentLevel; i++) extra_stmts << "    ";
                        writeTypeName(cg.typeResolver->resolveTypeWithExpr(arg.typeExpr),
                                      arg.typeExpr->pointer, extra_stmts);
                        extra_stmts << " " << arg.name << " = " << builtin << ";" << std::endl;
                    }
                }
            }
        } else if (_decl->shaderType != ast::ShaderDecl::Fragment) {
            for (unsigned arg_idx = 0; arg_idx < _decl->params.size(); arg_idx++) {
                auto &arg = _decl->params[arg_idx];
                if (arg.attributeName.has_value()) {
                    if (arg.attributeName.value() == ATTRIBUTE_VERTEX_ID) {
                        for (unsigned i = 0; i < cg.indentLevel; i++) extra_stmts << "    ";
                        writeTypeName(cg.typeResolver->resolveTypeWithExpr(arg.typeExpr),
                                      arg.typeExpr->pointer, extra_stmts);
                        if (_decl->shaderType == ast::ShaderDecl::Hull) {
                            extra_stmts << " " << arg.name << " = gl_InvocationID;" << std::endl;
                        } else if (_decl->shaderType == ast::ShaderDecl::Domain) {
                            extra_stmts << " " << arg.name << " = gl_PrimitiveID;" << std::endl;
                        } else {
                            extra_stmts << " " << arg.name << " = gl_VertexIndex;" << std::endl;
                        }
                        shader_entry.vertexShaderInputDesc.useVertexID = true;
                    } else if (arg.attributeName.value() == ATTRIBUTE_DOMAIN_LOCATION) {
                        /// §16 — the tessellator location. `gl_TessCoord` is
                        /// always a `vec3`; a tri domain takes it whole
                        /// (barycentric), a quad domain takes `.xy` (u,v).
                        for (unsigned i = 0; i < cg.indentLevel; i++) extra_stmts << "    ";
                        writeTypeName(cg.typeResolver->resolveTypeWithExpr(arg.typeExpr),
                                      arg.typeExpr->pointer, extra_stmts);
                        const bool isTri = _decl->tessDesc.domain
                                           == ast::ShaderDecl::TessellationDesc::Triangle;
                        extra_stmts << " " << arg.name << " = gl_TessCoord"
                                    << (isTri ? "" : ".xy") << ";" << std::endl;
                    }
                } else {
                    /// §16 Phase G — a hull/domain control-point-array input
                    /// (`CP cp[N]`, element = an internal struct) is the
                    /// post-vertex patch, which in GLSL rides the built-in
                    /// `gl_in[]` array. It gets NO `layout(location=N) in`
                    /// varying; instead record it so `emitMemberExpr` can
                    /// rewrite `cp[i].<field>` onto `gl_in[i].gl_Position`
                    /// (Position) at body-emission time. Only on the
                    /// tessellation stages (vertex reads its input by
                    /// VertexID, not a patch array).
                    bool isCPArray = false;
                    if ((_decl->shaderType == ast::ShaderDecl::Hull
                         || _decl->shaderType == ast::ShaderDecl::Domain)
                        && !arg.typeExpr->arrayDims.empty()) {
                        auto pred = [&](ast::StructDecl *d) { return d->name == arg.typeExpr->name; };
                        auto it = std::find_if(internalStructs.begin(), internalStructs.end(), pred);
                        if (it != internalStructs.end()) {
                            controlPointArrayParams.push_back(std::make_pair(arg.name, *it));
                            isCPArray = true;
                        }
                    }
                    if (!isCPArray) {
                        out << "layout(location = ";
                        out << arg_idx << ") in ";
                        writeTypeName(cg.typeResolver->resolveTypeWithExpr(arg.typeExpr),
                                      arg.typeExpr->pointer, out);
                        out << " " << arg.name << " ;" << std::endl;
                    }
                }
            }
        } else {
            for (auto &arg : _decl->params) {
                if (arg.attributeName.has_value()) {
                    /// Scalar fragment input — bridge from a gl_* builtin
                    /// into a local of the user's chosen name.
                    const char *builtin = nullptr;
                    bool needsUintCast = false;
                    if (arg.attributeName.value() == ATTRIBUTE_FRONTFACING) {
                        builtin = "gl_FrontFacing";
                    } else if (arg.attributeName.value() == ATTRIBUTE_SAMPLEINDEX) {
                        builtin = "gl_SampleID";
                        needsUintCast = true;
                    } else if (arg.attributeName.value() == ATTRIBUTE_INPUT_COVERAGE) {
                        builtin = "gl_SampleMaskIn[0]";
                        needsUintCast = true;
                    }
                    if (builtin != nullptr) {
                        for (unsigned i = 0; i < cg.indentLevel; i++) extra_stmts << "    ";
                        writeTypeName(cg.typeResolver->resolveTypeWithExpr(arg.typeExpr),
                                      arg.typeExpr->pointer, extra_stmts);
                        extra_stmts << " " << arg.name << " = ";
                        if (needsUintCast) {
                            extra_stmts << "uint(" << builtin << ")";
                        } else {
                            extra_stmts << builtin;
                        }
                        extra_stmts << ";" << std::endl;
                    }
                } else {
                    auto pred = [&](ast::StructDecl *d) {
                        return d->name == arg.typeExpr->name;
                    };
                    auto it = std::find_if(internalStructs.begin(), internalStructs.end(), pred);
                    if (it != internalStructs.end()) {
                        internalStructVarMap.push_back(std::make_pair(arg.name, *it));
                    }
                }
            }
        }

        /// `void main(){` is the function signature in GLSL — closing brace
        /// is part of the body emission.
        out << "void main(){" << std::endl;
    }

    void GLSLTarget::emitShaderEntryBody(CodeGen &cg,
                                         ast::ShaderDecl *_decl,
                                         omegasl_shader &/*meta*/,
                                         std::ostream &out) {
        /// Flush attribute-bridge locals at the top of main().
        out << extra_stmts.str() << std::endl;

        /// §2a — mesh shaders must call `SetMeshOutputsEXT(numVertices,
        /// numPrimitives)` once before any write to the mesh output
        /// arrays; reads or writes that happen first are undefined per
        /// `GL_EXT_mesh_shader`. Two paths:
        ///   (1) The user wrote `setMeshOutputs(nv, np)` somewhere in the
        ///       body — Sema stamped `meshHasUserSetMeshOutputsCall` on the
        ///       ShaderDecl. Skip the auto-emit; the body walk will lower
        ///       their call to `SetMeshOutputsEXT(nv, np)` in place
        ///       (see `renameBuiltin` for the GLSL name). Emitting both
        ///       would issue two `SetMeshOutputsEXT` calls, which the
        ///       extension explicitly forbids.
        ///   (2) No user call — the front-end has no implicit runtime
        ///       "actual" count, so we lock it to the descriptor's declared
        ///       maxima up front. Correctness without forcing the user to
        ///       think about it.
        if (_decl->shaderType == ast::ShaderDecl::Mesh
            && !_decl->meshHasUserSetMeshOutputsCall) {
            for (unsigned i = 0; i < cg.indentLevel; i++) out << "    ";
            out << "SetMeshOutputsEXT(" << meshMaxVertices << ", "
                << meshMaxPrimitives << ");" << std::endl;
        }

        /// Custom body loop with RETURN_DECL rerouting for fragment-output
        /// struct returns and hull/domain `gl_Position` writes.
        for (auto stmt_it = _decl->block->body.begin(); stmt_it != _decl->block->body.end(); stmt_it++) {
            auto stmt = *stmt_it;
            /// §6.1 — `threadgroup` decls are hoisted to file scope by
            /// `emitThreadgroupGlobals` as `shared`; skip them in the body.
            if (stmt->type == VAR_DECL && ((ast::VarDecl *)stmt)->isThreadgroup) {
                continue;
            }
            if (stmt->type == RETURN_DECL) {
                for (unsigned i = 0; i < cg.indentLevel; i++) {
                    out << "    ";
                }
                auto _return_stmt = (ast::ReturnDecl *)stmt;
                if (_return_stmt->expr && _decl->shaderType == ast::ShaderDecl::Fragment
                    && fragmentOutputStruct != nullptr) {
                    /// Fragment-output-struct return: per-field stores
                    /// happened earlier via member-expr routing. For
                    /// `OutputCoverage`, the per-field store wrote to a
                    /// synthetic `uint <struct>_<field>` local; flush the
                    /// `int(...)` cast back into `gl_SampleMask[0]` here so
                    /// the GLSL `int = uint` mismatch never surfaces.
                    for (auto &f : fragmentOutputStruct->fields) {
                        if (f.attributeName.has_value()
                            && f.attributeName.value() == ATTRIBUTE_OUTPUT_COVERAGE) {
                            out << "gl_SampleMask[0] = int("
                                << fragmentOutputStruct->name << "_" << f.name
                                << ");" << std::endl;
                            for (unsigned i = 0; i < cg.indentLevel; i++) out << "    ";
                        }
                    }
                    out << "return;" << std::endl;
                } else if (_return_stmt->expr && _decl->shaderType == ast::ShaderDecl::Fragment) {
                    out << activeReturnReplacement << " = ";
                    cg.generateExpr(_return_stmt->expr);
                    out << ";" << std::endl;
                } else if (_return_stmt->expr && (_decl->shaderType == ast::ShaderDecl::Hull
                                                  || _decl->shaderType == ast::ShaderDecl::Domain)) {
                    /// Hull/Domain: main() is void in GLSL. When the returned
                    /// value is an `internal`-struct local (e.g. a domain's
                    /// `DomainOut` whose `Position` field already routed to
                    /// gl_Position via member-expr rewriting), the write has
                    /// already happened — emit a bare `return`. Otherwise (a
                    /// plain hull-output struct), assign its Position field to
                    /// the stage's position output (`gl_out[…]` / `gl_Position`).
                    bool isInternalStructReturn = false;
                    if (_return_stmt->expr->type == ID_EXPR) {
                        auto *idExpr = (ast::IdExpr *)_return_stmt->expr;
                        isInternalStructReturn = std::any_of(
                            internalStructVarMap.begin(), internalStructVarMap.end(),
                            [&](std::pair<OmegaCommon::String, ast::StructDecl *> &p) {
                                return p.first == idExpr->id;
                            });
                    }
                    if (isInternalStructReturn) {
                        /// The per-CP / per-vertex position was already routed
                        /// to its builtin by the member-expr writes. A hull's
                        /// tess-factor epilogue (`if (gl_InvocationID == 0) …`)
                        /// still has to run *after* this point, so a hull emits
                        /// no mid-body `return;` (it would make the epilogue
                        /// unreachable); a domain has no epilogue, so a bare
                        /// `return;` is correct there.
                        if (_decl->shaderType != ast::ShaderDecl::Hull) {
                            out << "return;" << std::endl;
                        }
                    } else {
                        OmegaCommon::String posField;
                        auto retTypeName = _decl->returnType->name;
                        auto structIt = structDeclMap.find(retTypeName);
                        if (structIt != structDeclMap.end()) {
                            for (auto &f : structIt->second->fields) {
                                if (f.attributeName.has_value()
                                    && f.attributeName.value() == ATTRIBUTE_POSITION) {
                                    posField = f.name;
                                    break;
                                }
                            }
                            if (posField.empty() && structIt->second->fields.size() == 1) {
                                posField = structIt->second->fields[0].name;
                            }
                        }
                        if (_decl->shaderType == ast::ShaderDecl::Hull) {
                            out << "gl_out[gl_InvocationID].gl_Position = ";
                        } else {
                            out << "gl_Position = ";
                        }
                        cg.generateExpr(_return_stmt->expr);
                        if (!posField.empty()) {
                            out << "." << posField;
                        }
                        out << ";" << std::endl;
                    }
                } else if (_return_stmt->expr) {
                    /// If returning an internal struct variable, emit bare
                    /// return since fields were already assigned via
                    /// member expressions.
                    bool isInternalStructReturn = false;
                    if (_return_stmt->expr->type == ID_EXPR) {
                        auto *idExpr = (ast::IdExpr *)_return_stmt->expr;
                        isInternalStructReturn = std::any_of(
                            internalStructVarMap.begin(), internalStructVarMap.end(),
                            [&](std::pair<OmegaCommon::String, ast::StructDecl *> &p) {
                                return p.first == idExpr->id;
                            });
                    }
                    if (isInternalStructReturn) {
                        out << "return;" << std::endl;
                    } else {
                        out << "return ";
                        cg.generateExpr(_return_stmt->expr);
                        out << ";" << std::endl;
                    }
                } else {
                    out << "return;" << std::endl;
                }
            } else {
                /// Non-return statements go through the shared, injection-aware
                /// emitter (as the HLSL and MSL bodies already do — see
                /// `CodeGen::emitStatementLine`) so a builtin that queues
                /// pre-statements — the inline ray-query `intersect` /
                /// `ray_query_init` lowering — gets its `pendingStatements`
                /// flushed ahead of the statement instead of silently dropped.
                /// `emitStatementLine` owns the indent, the trailing `;`, and
                /// the newline; output is byte-identical to the old inline
                /// emission when nothing is queued.
                cg.emitStatementLine(stmt);
            }
        }
        /// §16 — hull tess-factor epilogue. The patchfn runs once per patch
        /// (guarded on `gl_InvocationID == 0`) and its factor fields drive the
        /// `gl_TessLevelOuter[]` / `gl_TessLevelInner[]` builtin arrays:
        ///   tri  — Outer[0..2] from edges, Inner[0] from inside (1 slot)
        ///   quad — Outer[0..3] from edges, Inner[0..1] from inside (2 slots)
        if (_decl->shaderType == ast::ShaderDecl::Hull) {
            OmegaCommon::String patchStructName, edgeField, insideField;
            bool edgeArr = false, insideArr = false;
            for (auto *fd : cg.userFuncDecls) {
                if (fd->name != _decl->tessDesc.patchFn) continue;
                patchStructName = fd->returnType->name;
                auto sit = structDeclMap.find(patchStructName);
                if (sit != structDeclMap.end()) {
                    for (auto &f : sit->second->fields) {
                        if (!f.attributeName.has_value()) continue;
                        if (f.attributeName.value() == ATTRIBUTE_TESS_FACTOR) {
                            edgeField = f.name;
                            edgeArr = !f.typeExpr->arrayDims.empty();
                        } else if (f.attributeName.value() == ATTRIBUTE_INSIDE_TESS_FACTOR) {
                            insideField = f.name;
                            insideArr = !f.typeExpr->arrayDims.empty();
                        }
                    }
                }
                break;
            }
            const bool isTri = _decl->tessDesc.domain
                               == ast::ShaderDecl::TessellationDesc::Triangle;
            const unsigned edgeCount = isTri ? 3u : 4u;
            out << "    if (gl_InvocationID == 0) {" << std::endl;
            out << "        " << patchStructName << " osl_pc = "
                << cg.spellUserFuncName(_decl->tessDesc.patchFn) << "();" << std::endl;
            for (unsigned i = 0; i < edgeCount; i++) {
                out << "        gl_TessLevelOuter[" << i << "] = osl_pc." << edgeField
                    << (edgeArr ? ("[" + std::to_string(i) + "]") : "") << ";" << std::endl;
            }
            const unsigned insideCount = isTri ? 1u : 2u;
            for (unsigned j = 0; j < insideCount; j++) {
                out << "        gl_TessLevelInner[" << j << "] = osl_pc." << insideField
                    << (insideArr ? ("[" + std::to_string(j) + "]") : "") << ";" << std::endl;
            }
            out << "    }" << std::endl;
        }

        cg.indentLevel -= 1;
        out << "}" << std::endl;

        /// Reset per-shader state — must happen after body emission so
        /// MEMBER_EXPR rerouting still works during the body.
        internalStructVarMap.clear();
        activeReturnReplacement.clear();
        fragmentOutputStruct = nullptr;
        meshVertsParamName.clear();
        meshIndicesParamName.clear();
        meshVertsStructDecl = nullptr;
        meshMaxVertices = 0;
        meshMaxPrimitives = 0;
        payloadParamName.clear();
        controlPointArrayParams.clear();
    }

    void GLSLTarget::writeInternalFieldRef(const ast::AttributedFieldDecl &field,
                                           const OmegaCommon::String &structName,
                                           std::ostream &out) const {
        if (!field.attributeName.has_value()) {
            out << structName << "_" << field.name;
            return;
        }
        const auto &attr = field.attributeName.value();
        if (attr == ATTRIBUTE_POSITION) {
            /// A `Position`-semantic field maps to the stage's position
            /// builtin. Vertex/Domain *write* it through `gl_Position`
            /// (clip space). A Hull (tessellation-control) writes its
            /// per-output-control-point position into the built-in
            /// `gl_out[gl_InvocationID].gl_Position` array member — there is
            /// no bare `gl_Position` in a `.tesc`. A fragment shader *reads*
            /// it as the interpolated window-space coordinate, which in GLSL
            /// is the read-only `gl_FragCoord` builtin — `gl_Position` does
            /// not exist in the fragment stage. This mirrors HLSL
            /// `SV_Position` and Metal `[[position]]`, where the same
            /// semantic is reinterpreted per stage by the downstream compiler.
            out << (currentShaderType == ast::ShaderDecl::Fragment ? "gl_FragCoord"
                    : currentShaderType == ast::ShaderDecl::Hull   ? "gl_out[gl_InvocationID].gl_Position"
                                                                   : "gl_Position");
        } else if (attr == ATTRIBUTE_COLOR && field.attributeIndex.has_value()) {
            out << "_outColor" << field.attributeIndex.value();
        } else if (attr == ATTRIBUTE_DEPTH) {
            out << "gl_FragDepth";
        } else if (attr == ATTRIBUTE_CLIP_DISTANCE) {
            /// §1.7 — clip distance routes to the `gl_ClipDistance[]` builtin
            /// array. A field write `r.clip[i] = x` becomes
            /// `gl_ClipDistance[i] = x` (the member reroute supplies the base,
            /// the user's index supplies the element).
            out << "gl_ClipDistance";
        } else if (attr == ATTRIBUTE_CULL_DISTANCE) {
            out << "gl_CullDistance";
        } else if (attr == ATTRIBUTE_OUTPUT_COVERAGE) {
            /// `gl_SampleMask[]` is `int[]`, but OmegaSL types this field as
            /// `uint`. We can't cast an lvalue, so route to a synthetic uint
            /// local of the form `<struct>_<field>`; `emitShaderEntryHeader`
            /// declares it and `emitShaderEntryBody` flushes
            /// `gl_SampleMask[0] = int(<struct>_<field>);` before each
            /// fragment-output-struct return.
            out << structName << "_" << field.name;
        } else {
            /// Bare Color, TexCoord, ... — interstage varying.
            out << structName << "_" << field.name;
        }
    }

    void GLSLTarget::emitMemberExpr(CodeGen &cg, ast::MemberExpr *expr, std::ostream &out) {
        /// Fragment-output struct rerouting: when the LHS is one of the
        /// internal-struct vars captured during shader-entry processing,
        /// emit the field's GLSL backing identifier (gl_Position,
        /// _outColorN, gl_FragDepth, ...) instead of `lhs.field`.
        if (expr->lhs->type == ID_EXPR) {
            auto *_id_expr = (ast::IdExpr *)expr->lhs;
            auto it = std::find_if(internalStructVarMap.begin(), internalStructVarMap.end(),
                                   [&](std::pair<OmegaCommon::String, ast::StructDecl *> &p) {
                                       return p.first == _id_expr->id;
                                   });
            if (it != internalStructVarMap.end()) {
                for (auto &f : it->second->fields) {
                    if (f.name == expr->rhs_id) {
                        writeInternalFieldRef(f, it->second->name, out);
                        return;
                    }
                }
            }
        }
        /// §16 Phase G — tessellation control-point-array read: `cp[i].field`
        /// on a hull/domain reads the built-in `gl_in[]` patch array. A
        /// `Position`-semantic field maps to `gl_in[i].gl_Position`; any other
        /// field maps to its per-field `in` varying array `<struct>_<field>[i]`
        /// (Position-only patches — the current tests — never hit that arm).
        /// Detect the shape: LHS is INDEX_EXPR whose base identifier is a
        /// recorded control-point-array parameter.
        if (expr->lhs->type == INDEX_EXPR && !controlPointArrayParams.empty()) {
            auto *_idx_expr = (ast::IndexExpr *)expr->lhs;
            if (_idx_expr->lhs->type == ID_EXPR) {
                auto *_id_expr = (ast::IdExpr *)_idx_expr->lhs;
                auto cpIt = std::find_if(controlPointArrayParams.begin(), controlPointArrayParams.end(),
                                         [&](std::pair<OmegaCommon::String, ast::StructDecl *> &p) {
                                             return p.first == _id_expr->id;
                                         });
                if (cpIt != controlPointArrayParams.end()) {
                    for (auto &f : cpIt->second->fields) {
                        if (f.name != expr->rhs_id) continue;
                        bool isPosition = f.attributeName.has_value()
                            && f.attributeName.value() == ATTRIBUTE_POSITION;
                        if (isPosition) {
                            out << "gl_in[";
                            cg.generateExpr(_idx_expr->idx_expr);
                            out << "].gl_Position";
                        } else {
                            out << cpIt->second->name << "_" << f.name << "[";
                            cg.generateExpr(_idx_expr->idx_expr);
                            out << "]";
                        }
                        return;
                    }
                }
            }
        }
        /// §2a — mesh `out vertices` slot access: `verts[i].field` writes
        /// rewrite onto either `gl_MeshVerticesEXT[i].gl_Position` (the
        /// `Position` semantic only) or the per-field arrayed varying
        /// `<struct>_<field>[i]` (every other field). Detect the shape:
        /// LHS is INDEX_EXPR whose base is the verts param's identifier.
        if (expr->lhs->type == INDEX_EXPR && meshVertsStructDecl != nullptr
            && !meshVertsParamName.empty()) {
            auto *_idx_expr = (ast::IndexExpr *)expr->lhs;
            if (_idx_expr->lhs->type == ID_EXPR) {
                auto *_id_expr = (ast::IdExpr *)_idx_expr->lhs;
                if (_id_expr->id == meshVertsParamName) {
                    for (auto &f : meshVertsStructDecl->fields) {
                        if (f.name != expr->rhs_id) continue;
                        bool isPosition = f.attributeName.has_value()
                            && f.attributeName.value() == ATTRIBUTE_POSITION;
                        if (isPosition) {
                            out << "gl_MeshVerticesEXT[";
                            cg.generateExpr(_idx_expr->idx_expr);
                            out << "].gl_Position";
                        } else {
                            out << meshVertsStructDecl->name << "_" << f.name << "[";
                            cg.generateExpr(_idx_expr->idx_expr);
                            out << "]";
                        }
                        return;
                    }
                }
            }
        }
        /// Default emission: same as the abstract Target default.
        cg.generateExpr(expr->lhs);
        out << "." << expr->rhs_id;
    }

    void GLSLTarget::emitIndexExpr(CodeGen &cg, ast::IndexExpr *expr, std::ostream &out) {
        /// §2a — mesh `out indices` slot access: `tris[i] = uvec3(...)`
        /// rewrites onto `gl_PrimitiveTriangleIndicesEXT[i]` (or the line
        /// / point variant, picked from the topology recorded at entry-
        /// header emission). The shape is INDEX_EXPR whose base
        /// identifier matches the indices param's name. Everything else
        /// falls through to the default `lhs[idx]` emission.
        if (!meshIndicesParamName.empty() && expr->lhs->type == ID_EXPR) {
            auto *_id_expr = (ast::IdExpr *)expr->lhs;
            if (_id_expr->id == meshIndicesParamName) {
                const char *builtin = (meshTopology == ast::ShaderDecl::MeshDesc::Triangle)
                                        ? "gl_PrimitiveTriangleIndicesEXT"
                                      : (meshTopology == ast::ShaderDecl::MeshDesc::Line)
                                        ? "gl_PrimitiveLineIndicesEXT"
                                        : "gl_PrimitivePointIndicesEXT";
                out << builtin << "[";
                cg.generateExpr(expr->idx_expr);
                out << "]";
                return;
            }
        }
        cg.generateExpr(expr->lhs);
        out << "[";
        cg.generateExpr(expr->idx_expr);
        out << "]";
    }

    /// True for the 16-bit scalar builtins whose GLSL spellings
    /// (`float16_t` / `int16_t` / `uint16_t`) the explicit-arithmetic-types
    /// extension will not implicitly construct from a default-typed literal.
    static bool isNarrowScalarType(ast::Type *t) {
        return t == ast::builtins::half_type
            || t == ast::builtins::short_type
            || t == ast::builtins::ushort_type;
    }

    bool GLSLTarget::tryEmitLiteralExpr(CodeGen &cg, ast::LiteralExpr *expr, std::ostream &out) {
        /// Sema stamps `resolvedType` to the destination slot's type when a
        /// numeric literal is coerced into it (var-decl initializer §3.6,
        /// assignment RHS §3.2). Vector forms reach a 16-bit slot through a
        /// `make_*N` constructor, which converts; a bare scalar literal does
        /// not, and GLSL has no implicit float→float16_t / int→int16_t cast.
        /// Wrap it in the target-type constructor so `half s = 0.5;` emits
        /// `float16_t s = float16_t(0.5);`.
        if (expr->resolvedType == nullptr) {
            return false;
        }
        auto *ty = cg.typeResolver->resolveTypeWithExpr(expr->resolvedType);
        if (ty == nullptr || !isNarrowScalarType(ty)) {
            return false;
        }
        writeTypeName(ty, false, out);
        out << "(";
        cg.emitLiteralValue(expr, out);
        out << ")";
        return true;
    }

    void GLSLTarget::emitResourceBinding(CodeGen &cg,
                                         ast::ResourceDecl *res_decl,
                                         ast::ShaderDecl *shader,
                                         omegasl_shader_layout_desc_io_mode ioMode,
                                         std::ostream &out,
                                         omegasl_shader_layout_desc &layoutDesc) {
        unsigned set = (shader->shaderType == ast::ShaderDecl::Fragment) ? 1 : 0;
        auto type_ = cg.typeResolver->resolveTypeWithExpr(res_decl->typeExpr);

        /// §2.2/§10.2 — a push constant lives in a `layout(push_constant)`
        /// block, not in a descriptor set, so it must NOT consume a `binding`
        /// number (doing so would leave a hole in the otherwise-contiguous
        /// descriptor bindings of the set). Set in the push-constant branch
        /// and consulted at the shared tail to skip the `++binding`.
        bool isPushConstant = false;

        if (type_ == ast::builtins::buffer_type) {
            layoutDesc.type = OMEGASL_SHADER_BUFFER_DESC;
            out << "layout(std430,set = " << set << ",binding = " << binding << ") ";
            out << "buffer "; writeGLSLIdent(res_decl->name, out); out << "_Layout" << std::endl;
            out << "{" << std::endl;
            writeTypeName(cg.typeResolver->resolveTypeWithExpr(res_decl->typeExpr->args[0]),
                          res_decl->typeExpr->args[0]->pointer, out);
            out << " "; writeGLSLIdent(res_decl->name, out); out << "[];" << std::endl;
            out << "};" << std::endl;
        } else if (type_ == ast::builtins::uniform_type) {
            /// §2.4 constant buffer — std140-laid-out `uniform` block with a
            /// single struct member named after the resource, so `name.field`
            /// resolves and there is no indexing (vs the std430 `buffer` block
            /// above, whose member is an unsized array).
            layoutDesc.type = OMEGASL_SHADER_UNIFORM_DESC;
            out << "layout(std140,set = " << set << ",binding = " << binding << ") ";
            out << "uniform "; writeGLSLIdent(res_decl->name, out); out << "_Layout" << std::endl;
            out << "{" << std::endl;
            writeTypeName(cg.typeResolver->resolveTypeWithExpr(res_decl->typeExpr->args[0]),
                          res_decl->typeExpr->args[0]->pointer, out);
            out << " "; writeGLSLIdent(res_decl->name, out); out << ";" << std::endl;
            out << "};" << std::endl;
        } else if (type_ == ast::builtins::push_constant_type) {
            /// §2.2/§10.2 push constant — a `layout(push_constant)` block, the
            /// one resource form that does NOT live in a descriptor set: it
            /// carries no `set`/`binding` and consumes no descriptor binding
            /// slot (Vulkan binds it through the pipeline layout's
            /// `VkPushConstantRange`, fed by `vkCmdPushConstants`). The block
            /// holds a single struct member named after the resource, so
            /// `name.field` resolves with no indexing (same value-access shape
            /// as the uniform block above). std430 layout — push constants are
            /// the tight, 128-byte-budgeted path; the Phase-B host-side writer
            /// must pack std430 to match (distinct from uniform's std140).
            layoutDesc.type = OMEGASL_SHADER_PUSH_CONSTANT_DESC;
            isPushConstant = true;
            out << "layout(push_constant,std430) ";
            out << "uniform "; writeGLSLIdent(res_decl->name, out); out << "_Layout" << std::endl;
            out << "{" << std::endl;
            writeTypeName(cg.typeResolver->resolveTypeWithExpr(res_decl->typeExpr->args[0]),
                          res_decl->typeExpr->args[0]->pointer, out);
            out << " "; writeGLSLIdent(res_decl->name, out); out << ";" << std::endl;
            out << "};" << std::endl;
        } else if (type_ == ast::builtins::texture1d_type) {
            layoutDesc.type = OMEGASL_SHADER_TEXTURE1D_DESC;
            out << "layout(set = " << set << ",binding = " << binding;
            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                out << ")";
            } else {
                out << ",rgba32f)";
            }
            out << " uniform ";
            out << (ioMode == OMEGASL_SHADER_DESC_IO_IN ? "texture1D" : "image1D");
            out << " "; writeGLSLIdent(res_decl->name, out); out << ";" << std::endl;
        } else if (type_ == ast::builtins::texture2d_type) {
            layoutDesc.type = OMEGASL_SHADER_TEXTURE2D_DESC;
            out << "layout(set = " << set << ",binding = " << binding;
            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                out << ")";
            } else {
                out << ",rgba32f)";
            }
            out << " uniform ";
            out << (ioMode == OMEGASL_SHADER_DESC_IO_IN ? "texture2D" : "image2D");
            out << " "; writeGLSLIdent(res_decl->name, out); out << ";" << std::endl;
        } else if (type_ == ast::builtins::texture3d_type) {
            layoutDesc.type = OMEGASL_SHADER_TEXTURE3D_DESC;
            out << "layout(set = " << set << ",binding = " << binding;
            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                out << ")";
            } else {
                out << ",rgba32f)";
            }
            out << " uniform ";
            out << (ioMode == OMEGASL_SHADER_DESC_IO_IN ? "texture3D" : "image3D");
            out << " "; writeGLSLIdent(res_decl->name, out); out << ";" << std::endl;
        } else if (type_ == ast::builtins::texture1d_array_type) {
            layoutDesc.type = OMEGASL_SHADER_TEXTURE1D_ARRAY_DESC;
            out << "layout(set = " << set << ",binding = " << binding;
            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                out << ")";
            } else {
                out << ",rgba32f)";
            }
            out << " uniform ";
            out << (ioMode == OMEGASL_SHADER_DESC_IO_IN ? "texture1DArray" : "image1DArray");
            out << " "; writeGLSLIdent(res_decl->name, out); out << ";" << std::endl;
        } else if (type_ == ast::builtins::texture2d_array_type) {
            layoutDesc.type = OMEGASL_SHADER_TEXTURE2D_ARRAY_DESC;
            out << "layout(set = " << set << ",binding = " << binding;
            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                out << ")";
            } else {
                out << ",rgba32f)";
            }
            out << " uniform ";
            out << (ioMode == OMEGASL_SHADER_DESC_IO_IN ? "texture2DArray" : "image2DArray");
            out << " "; writeGLSLIdent(res_decl->name, out); out << ";" << std::endl;
        } else if (type_ == ast::builtins::texturecube_type) {
            /// Cube `write` is rejected by Sema, so the storage form
            /// (`imageCube`) is unreachable here. Emit the read-only form.
            layoutDesc.type = OMEGASL_SHADER_TEXTURECUBE_DESC;
            out << "layout(set = " << set << ",binding = " << binding;
            out << ") uniform textureCube ";
            writeGLSLIdent(res_decl->name, out); out << ";" << std::endl;
        } else if (type_ == ast::builtins::texturecube_array_type) {
            layoutDesc.type = OMEGASL_SHADER_TEXTURECUBE_ARRAY_DESC;
            out << "layout(set = " << set << ",binding = " << binding;
            out << ") uniform textureCubeArray ";
            writeGLSLIdent(res_decl->name, out); out << ";" << std::endl;
        } else if (type_ == ast::builtins::texture2d_ms_type) {
            /// MS textures are read-only with explicit sample-index access.
            /// `texture2DMS` is the separate (Vulkan-style) form; the
            /// read-side intrinsic emit pairs it with a sampler at use time
            /// to form `sampler2DMS`.
            layoutDesc.type = OMEGASL_SHADER_TEXTURE2D_MS_DESC;
            out << "layout(set = " << set << ",binding = " << binding;
            out << ") uniform texture2DMS ";
            writeGLSLIdent(res_decl->name, out); out << ";" << std::endl;
        } else if (type_ == ast::builtins::texture2d_ms_array_type) {
            layoutDesc.type = OMEGASL_SHADER_TEXTURE2D_MS_ARRAY_DESC;
            out << "layout(set = " << set << ",binding = " << binding;
            out << ") uniform texture2DMSArray ";
            writeGLSLIdent(res_decl->name, out); out << ";" << std::endl;
        } else if (type_ == ast::builtins::acceleration_structure_type) {
            /// Inline ray tracing (Raytracing plan §2.2). A TLAS binds as a
            /// read-only `uniform accelerationStructureEXT` descriptor
            /// (GL_EXT_ray_query, enabled in emitDefaultHeaders). Read-only, so
            /// no `image`/format form like the storage textures above.
            layoutDesc.type = OMEGASL_SHADER_ACCELERATION_STRUCTURE_DESC;
            out << "layout(set = " << set << ",binding = " << binding;
            out << ") uniform accelerationStructureEXT ";
            writeGLSLIdent(res_decl->name, out); out << ";" << std::endl;
        } else if (type_ == ast::builtins::sampler1d_type) {
            layoutDesc.type = res_decl->isStatic ? OMEGASL_SHADER_STATIC_SAMPLER1D_DESC
                                                 : OMEGASL_SHADER_SAMPLER1D_DESC;
            out << "layout(set = " << set << ",binding =" << binding << ") uniform sampler ";
            writeGLSLIdent(res_decl->name, out); out << ";" << std::endl;
        } else if (type_ == ast::builtins::sampler2d_type) {
            layoutDesc.type = res_decl->isStatic ? OMEGASL_SHADER_STATIC_SAMPLER2D_DESC
                                                 : OMEGASL_SHADER_SAMPLER2D_DESC;
            out << "layout(set = " << set << ",binding =" << binding << ") uniform sampler ";
            writeGLSLIdent(res_decl->name, out); out << ";" << std::endl;
        } else if (type_ == ast::builtins::sampler3d_type) {
            layoutDesc.type = res_decl->isStatic ? OMEGASL_SHADER_STATIC_SAMPLER3D_DESC
                                                 : OMEGASL_SHADER_SAMPLER3D_DESC;
            out << "layout(set = " << set << ",binding =" << binding << ") uniform sampler ";
            writeGLSLIdent(res_decl->name, out); out << ";" << std::endl;
        } else if (type_ == ast::builtins::samplercube_type) {
            layoutDesc.type = res_decl->isStatic ? OMEGASL_SHADER_STATIC_SAMPLERCUBE_DESC
                                                 : OMEGASL_SHADER_SAMPLERCUBE_DESC;
            out << "layout(set = " << set << ",binding =" << binding << ") uniform sampler ";
            writeGLSLIdent(res_decl->name, out); out << ";" << std::endl;
        }

        layoutDesc.location = res_decl->registerNumber;
        /// A push constant has no descriptor binding; leave gpu_relative_loc 0
        /// (unused by the Vulkan push-constant path, which keys off offset).
        layoutDesc.gpu_relative_loc = isPushConstant ? 0 : binding;
        layoutDesc.offset = 0;
        layoutDesc.io_mode = ioMode;
        if (res_decl->isStatic) {
            layoutDesc.sampler_desc.max_anisotropy = res_decl->staticSamplerDesc->maxAnisotropy;
            layoutDesc.sampler_desc.filter = res_decl->staticSamplerDesc->filter;
            layoutDesc.sampler_desc.u_address_mode = res_decl->staticSamplerDesc->uAddressMode;
            layoutDesc.sampler_desc.v_address_mode = res_decl->staticSamplerDesc->vAddressMode;
            layoutDesc.sampler_desc.w_address_mode = res_decl->staticSamplerDesc->wAddressMode;
        }

        if (!isPushConstant) {
            ++binding;
        }
    }

    OmegaCommon::StrRef GLSLTarget::discardStatement() { return "discard"; }

    void GLSLTarget::writeCast(CodeGen &cg, ast::TypeExpr *t, std::ostream &out) {
        writeTypeName(cg.typeResolver->resolveTypeWithExpr(t), t->pointer, out);
    }

    /// §3.7 — GLSL spells `out` / `inout` as a prefix keyword (same shape
    /// as HLSL). `in` is the default and is omitted to keep generated
    /// source byte-identical for unqualified params.
    void GLSLTarget::writeFuncParam(CodeGen &cg,
                                    const ast::AttributedFieldDecl &param,
                                    std::ostream &out) {
        /// §3.6 — `const` param (`const T name`, with `in` implicit). Sema
        /// guarantees it never co-occurs with `out` / `inout`.
        if (param.isConst) {
            out << "const ";
        }
        if (param.access == ast::AttributedFieldDecl::Out) {
            out << "out ";
        } else if (param.access == ast::AttributedFieldDecl::Inout) {
            out << "inout ";
        }
        writeTypeName(cg.typeResolver->resolveTypeWithExpr(param.typeExpr),
                      param.typeExpr->pointer, out);
        out << " ";
        writeIdentifier(param.name, out);
    }

    /// GLSL has no raw pointer types — `&expr` and `*expr` are not valid
    /// source-level constructs. The current GLSLCodeGen still emits them
    /// at the source level if it sees a POINTER_EXPR, producing invalid
    /// GLSL. Sema can consult this flag to refuse pointer expressions
    /// upstream when targeting GLSL.
    bool GLSLTarget::supportsPointerExpr() const { return false; }

    OmegaCommon::StrRef GLSLTarget::renameBuiltin(OmegaCommon::StrRef name) {
        if (name == BUILTIN_LERP)  return "mix";
        if (name == BUILTIN_FRAC)  return "fract";
        if (name == BUILTIN_ATAN2) return "atan";
        /// §5.1: GLSL spells `rsqrt` as `inversesqrt`. `saturate` and `fmod`
        /// have no GLSL equivalent at all — those are rewritten in
        /// `tryEmitBuiltinCall` rather than renamed.
        if (name == BUILTIN_RSQRT) return "inversesqrt";
        /// §5.3 — `bitfieldReverse` is the GLSL name and returns the
        /// operand type (T → T native), so a plain rename preserves the
        /// operand-typed contract. `countbits` is *not* a plain rename:
        /// GLSL's `bitCount` returns a signed iN regardless of operand
        /// signedness, so it is handled in `tryEmitBuiltinCall` with a
        /// cast back to the operand type.
        if (name == BUILTIN_REVERSEBITS) return "bitfieldReverse";
        /// §5.4 — derivatives. GLSL spells the basic ops `dFdx` / `dFdy` /
        /// `fwidth`. The coarse/fine variants are core in GLSL 4.50
        /// (originally `GL_ARB_derivative_control`). The preamble targets
        /// `#version 450` already, so no extension line is needed.
        if (name == BUILTIN_DDX)            return "dFdx";
        if (name == BUILTIN_DDY)            return "dFdy";
        if (name == BUILTIN_DDX_COARSE)     return "dFdxCoarse";
        if (name == BUILTIN_DDX_FINE)       return "dFdxFine";
        if (name == BUILTIN_DDY_COARSE)     return "dFdyCoarse";
        if (name == BUILTIN_DDY_FINE)       return "dFdyFine";
        if (name == BUILTIN_FWIDTH_COARSE)  return "fwidthCoarse";
        if (name == BUILTIN_FWIDTH_FINE)    return "fwidthFine";
        /// `fwidth` itself passes through unchanged on GLSL.
        /// §6.2 — compute barriers. `barrier()` is the execution +
        /// shared-memory control barrier; `memoryBarrier()` is the
        /// device-memory-only ordering barrier (no execution sync).
        if (name == BUILTIN_THREADGROUP_BARRIER) return "barrier";
        if (name == BUILTIN_DEVICE_BARRIER)      return "memoryBarrier";
        /// §2a follow-up — mesh-shader runtime output count. The GLSL spelling
        /// for `setMeshOutputs(nv, np)` is `SetMeshOutputsEXT(nv, np)` (both
        /// counts, mandatory before any output-array write). Sema already
        /// gated the call to mesh-stage and stamped
        /// `meshHasUserSetMeshOutputsCall` on the ShaderDecl so
        /// `emitShaderEntryBody` knows to suppress the auto-emit; here the
        /// shared `(args)` print does the actual lowering.
        if (name == BUILTIN_SET_MESH_OUTPUTS)    return "SetMeshOutputsEXT";
        if (name == BUILTIN_MAKE_FLOAT2)   return "vec2";
        if (name == BUILTIN_MAKE_FLOAT3)   return "vec3";
        if (name == BUILTIN_MAKE_FLOAT4)   return "vec4";
        if (name == BUILTIN_MAKE_BOOL2)    return "bvec2";
        if (name == BUILTIN_MAKE_BOOL3)    return "bvec3";
        if (name == BUILTIN_MAKE_BOOL4)    return "bvec4";
        if (name == BUILTIN_MAKE_INT2)     return "ivec2";
        if (name == BUILTIN_MAKE_INT3)     return "ivec3";
        if (name == BUILTIN_MAKE_INT4)     return "ivec4";
        if (name == BUILTIN_MAKE_UINT2)    return "uvec2";
        if (name == BUILTIN_MAKE_UINT3)    return "uvec3";
        if (name == BUILTIN_MAKE_UINT4)    return "uvec4";
        /// §4.1 / §4.2 — `f16vec*`/`i16vec*`/`u16vec*`/`i64vec*`/`u64vec*`
        /// are the GLSL constructor spellings.
        if (name == BUILTIN_MAKE_HALF2)    return "f16vec2";
        if (name == BUILTIN_MAKE_HALF3)    return "f16vec3";
        if (name == BUILTIN_MAKE_HALF4)    return "f16vec4";
        if (name == BUILTIN_MAKE_SHORT2)   return "i16vec2";
        if (name == BUILTIN_MAKE_SHORT3)   return "i16vec3";
        if (name == BUILTIN_MAKE_SHORT4)   return "i16vec4";
        if (name == BUILTIN_MAKE_USHORT2)  return "u16vec2";
        if (name == BUILTIN_MAKE_USHORT3)  return "u16vec3";
        if (name == BUILTIN_MAKE_USHORT4)  return "u16vec4";
        if (name == BUILTIN_MAKE_LONG2)    return "i64vec2";
        if (name == BUILTIN_MAKE_LONG3)    return "i64vec3";
        if (name == BUILTIN_MAKE_LONG4)    return "i64vec4";
        if (name == BUILTIN_MAKE_ULONG2)   return "u64vec2";
        if (name == BUILTIN_MAKE_ULONG3)   return "u64vec3";
        if (name == BUILTIN_MAKE_ULONG4)   return "u64vec4";
        /// §4.3 — GLSL double-precision vectors.
        if (name == BUILTIN_MAKE_DOUBLE2)  return "dvec2";
        if (name == BUILTIN_MAKE_DOUBLE3)  return "dvec3";
        if (name == BUILTIN_MAKE_DOUBLE4)  return "dvec4";
        if (name == BUILTIN_MAKE_FLOAT2X2) return "mat2";
        if (name == BUILTIN_MAKE_FLOAT3X3) return "mat3";
        if (name == BUILTIN_MAKE_FLOAT4X4) return "mat4";
        if (name == BUILTIN_MAKE_FLOAT2X3) return "mat2x3";
        if (name == BUILTIN_MAKE_FLOAT2X4) return "mat2x4";
        if (name == BUILTIN_MAKE_FLOAT3X2) return "mat3x2";
        if (name == BUILTIN_MAKE_FLOAT3X4) return "mat3x4";
        if (name == BUILTIN_MAKE_FLOAT4X2) return "mat4x2";
        if (name == BUILTIN_MAKE_FLOAT4X3) return "mat4x3";
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
        if(texTy == builtins::texture1d_array_type) return "ivec2";
        if(texTy == builtins::texture2d_array_type) return "ivec3";
        if(texTy == builtins::texture2d_ms_type) return "ivec2";
        if(texTy == builtins::texture2d_ms_array_type) return "ivec3";
        return nullptr;
    }

    static ast::Type *glslResolveTextureType(CodeGen &cg, ast::Expr *texArg){
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

    void GLSLTarget::emitTextureSample(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// GLSL Vulkan combined-sampler synthesis. The combined-sampler type
        /// is keyed off the *texture's* shape (cube, array, …) — the
        /// `sampler1d`/`sampler2d`/`samplercube` keyword on the OmegaSL
        /// sampler is just the configuration channel and doesn't carry
        /// array/cube-array shape on its own.
        auto *texTy = glslResolveTextureType(cg, _expr->args[1]);
        OmegaCommon::String samplerType;
        if(texTy == ast::builtins::texture1d_type){
            samplerType = "sampler1D";
        } else if(texTy == ast::builtins::texture1d_array_type){
            samplerType = "sampler1DArray";
        } else if(texTy == ast::builtins::texture2d_type){
            samplerType = "sampler2D";
        } else if(texTy == ast::builtins::texture2d_array_type){
            samplerType = "sampler2DArray";
        } else if(texTy == ast::builtins::texture3d_type){
            samplerType = "sampler3D";
        } else if(texTy == ast::builtins::texturecube_type){
            samplerType = "samplerCube";
        } else if(texTy == ast::builtins::texturecube_array_type){
            samplerType = "samplerCubeArray";
        } else {
            /// Fallback to the original sampler-keyed path for any
            /// unforeseen type — keeps generated source decisive even if
            /// upstream Sema regresses.
            auto samplerid_expr = (ast::IdExpr *)_expr->args[0];
            auto &sampler_res = *(cg.resourceStore.find(samplerid_expr->id));
            auto t = cg.typeResolver->resolveTypeWithExpr(sampler_res->typeExpr);
            if(t == ast::builtins::sampler1d_type) samplerType = "sampler1D";
            else if(t == ast::builtins::sampler2d_type) samplerType = "sampler2D";
            else if(t == ast::builtins::sampler3d_type) samplerType = "sampler3D";
            else if(t == ast::builtins::samplercube_type) samplerType = "samplerCube";
        }

        out << "texture(" << samplerType << "(";
        cg.generateExpr(_expr->args[1]);
        out << ",";
        cg.generateExpr(_expr->args[0]);
        out << "),";
        cg.generateExpr(_expr->args[2]);
        out << ")";
    }

    /// Pick the GLSL combined-sampler-type spelling for a `(sampler, texture)`
    /// pair, mirroring the table in `emitTextureSample`. Shared by every
    /// Phase 2.3 sample variant so the texture-shape → sampler-type mapping
    /// is centralized.
    static OmegaCommon::String glslSamplerTypeForTextureArg(CodeGen &cg, ast::Expr *texArg, ast::Expr *samplerArg){
        auto *texTy = glslResolveTextureType(cg, texArg);
        if(texTy == ast::builtins::texture1d_type) return "sampler1D";
        if(texTy == ast::builtins::texture1d_array_type) return "sampler1DArray";
        if(texTy == ast::builtins::texture2d_type) return "sampler2D";
        if(texTy == ast::builtins::texture2d_array_type) return "sampler2DArray";
        if(texTy == ast::builtins::texture3d_type) return "sampler3D";
        if(texTy == ast::builtins::texturecube_type) return "samplerCube";
        if(texTy == ast::builtins::texturecube_array_type) return "samplerCubeArray";
        /// Fallback: derive from the OmegaSL sampler type, matching the
        /// pre-Phase-2.3 behaviour of `emitTextureSample`.
        if(samplerArg && samplerArg->type == ID_EXPR){
            auto samplerid_expr = (ast::IdExpr *)samplerArg;
            auto sampler_it = cg.resourceStore.find(samplerid_expr->id);
            if(sampler_it != cg.resourceStore.end()){
                auto t = cg.typeResolver->resolveTypeWithExpr((*sampler_it)->typeExpr);
                if(t == ast::builtins::sampler1d_type) return "sampler1D";
                if(t == ast::builtins::sampler2d_type) return "sampler2D";
                if(t == ast::builtins::sampler3d_type) return "sampler3D";
                if(t == ast::builtins::samplercube_type) return "samplerCube";
            }
        }
        return "sampler2D";
    }

    void GLSLTarget::emitTextureSampleLOD(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// `textureLod(samplerND(t, s), coord, lod)`.
        auto samplerType = glslSamplerTypeForTextureArg(cg, _expr->args[1], _expr->args[0]);
        out << "textureLod(" << samplerType << "(";
        cg.generateExpr(_expr->args[1]);
        out << ",";
        cg.generateExpr(_expr->args[0]);
        out << "),";
        cg.generateExpr(_expr->args[2]);
        out << ",";
        cg.generateExpr(_expr->args[3]);
        out << ")";
    }

    void GLSLTarget::emitTextureSampleBias(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// `texture(samplerND(t, s), coord, bias)` — the fourth arg of GLSL
        /// `texture()` is a LOD bias when the implementation supports it.
        auto samplerType = glslSamplerTypeForTextureArg(cg, _expr->args[1], _expr->args[0]);
        out << "texture(" << samplerType << "(";
        cg.generateExpr(_expr->args[1]);
        out << ",";
        cg.generateExpr(_expr->args[0]);
        out << "),";
        cg.generateExpr(_expr->args[2]);
        out << ",";
        cg.generateExpr(_expr->args[3]);
        out << ")";
    }

    void GLSLTarget::emitTextureSampleGrad(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// `textureGrad(samplerND(t, s), coord, ddx, ddy)`.
        auto samplerType = glslSamplerTypeForTextureArg(cg, _expr->args[1], _expr->args[0]);
        out << "textureGrad(" << samplerType << "(";
        cg.generateExpr(_expr->args[1]);
        out << ",";
        cg.generateExpr(_expr->args[0]);
        out << "),";
        cg.generateExpr(_expr->args[2]);
        out << ",";
        cg.generateExpr(_expr->args[3]);
        out << ",";
        cg.generateExpr(_expr->args[4]);
        out << ")";
    }

    void GLSLTarget::emitTextureGather(CodeGen &cg, ast::CallExpr *_expr, int channel, std::ostream &out) {
        /// `textureGather(samplerND(t, s), coord [, comp])`. GLSL takes the
        /// channel selector as a trailing int (0=R, 1=G, 2=B, 3=A); when the
        /// 3-arg form is used, the channel defaults to 0 (R).
        auto samplerType = glslSamplerTypeForTextureArg(cg, _expr->args[1], _expr->args[0]);
        out << "textureGather(" << samplerType << "(";
        cg.generateExpr(_expr->args[1]);
        out << ",";
        cg.generateExpr(_expr->args[0]);
        out << "),";
        cg.generateExpr(_expr->args[2]);
        if(channel >= 0){
            out << "," << channel;
        }
        out << ")";
    }

    /// §5.3 — GLSL spelling of an integer scalar / vector type, used to
    /// cast `bitCount`'s signed result back to the operand's type.
    /// nullptr for any non-integer type (Sema rejects those upstream).
    static const char *glslIntTypeSpelling(ast::Type *t){
        using namespace ast::builtins;
        if(t == int_type)  return "int";
        if(t == int2_type) return "ivec2";
        if(t == int3_type) return "ivec3";
        if(t == int4_type) return "ivec4";
        if(t == uint_type)  return "uint";
        if(t == uint2_type) return "uvec2";
        if(t == uint3_type) return "uvec3";
        if(t == uint4_type) return "uvec4";
        return nullptr;
    }

    /// Sub-phase 1.5 — the low-level `ray_query_*` traversal family (GLSL,
    /// GL_EXT_ray_query). Returns true (and emits) when `name` is one of the
    /// family; false otherwise. Unlike HLSL/MSL these are free functions taking
    /// the `rayQueryEXT` (and a committed/candidate `bool`), not methods; the
    /// int-returning index accessors are wrapped in `uint(...)`.
    static bool glslEmitRayQuery(CodeGen &cg, ast::CallExpr *_expr,
                                 OmegaCommon::StrRef name, std::ostream &out) {
        if (name == BUILTIN_RAY_QUERY_INIT) {
            std::string q    = cg.renderExprToString(_expr->args[0]);
            std::string as   = cg.renderExprToString(_expr->args[1]);
            std::string ray  = cg.renderExprToString(_expr->args[2]);
            std::string mask = (_expr->args.size() == 4)
                ? ("uint(" + cg.renderExprToString(_expr->args[3]) + ")")
                : std::string("0xFFu");
            out << "rayQueryInitializeEXT(" << q << ", " << as << ", gl_RayFlagsNoneEXT, "
                << mask << ", " << ray << ".origin, " << ray << ".tmin, "
                << ray << ".direction, " << ray << ".tmax)";
            return true;
        }
        if (name == BUILTIN_RAY_QUERY_PROCEED) {
            out << "rayQueryProceedEXT("; cg.generateExpr(_expr->args[0]); out << ")";
            return true;
        }
        if (name == BUILTIN_RAY_QUERY_COMMIT) {
            out << "rayQueryConfirmIntersectionEXT("; cg.generateExpr(_expr->args[0]); out << ")";
            return true;
        }
        if (name == BUILTIN_RAY_QUERY_COMMITTED) {
            out << "(rayQueryGetIntersectionTypeEXT("; cg.generateExpr(_expr->args[0]);
            out << ", true) == gl_RayQueryCommittedIntersectionTriangleEXT)";
            return true;
        }
        /// The remaining accessors share the `rayQueryGetIntersection*EXT(q,
        /// committed)` shape — `committed` is `true` for the committed-hit
        /// accessors and `false` for the candidate ones.
        const char *fn = nullptr;   // the EXT accessor name
        const char *committed = nullptr; // "true" / "false"
        bool castUint = false;      // int-returning accessors wrap in uint()
        if (name == BUILTIN_RAY_QUERY_T)                 { fn = "rayQueryGetIntersectionTEXT"; committed = "true"; }
        else if (name == BUILTIN_RAY_QUERY_PRIMITIVE)    { fn = "rayQueryGetIntersectionPrimitiveIndexEXT"; committed = "true"; castUint = true; }
        else if (name == BUILTIN_RAY_QUERY_INSTANCE)     { fn = "rayQueryGetIntersectionInstanceIdEXT"; committed = "true"; castUint = true; }
        else if (name == BUILTIN_RAY_QUERY_BARYCENTRICS) { fn = "rayQueryGetIntersectionBarycentricsEXT"; committed = "true"; }
        else if (name == BUILTIN_RAY_QUERY_CANDIDATE_T)  { fn = "rayQueryGetIntersectionTEXT"; committed = "false"; }
        else if (name == BUILTIN_RAY_QUERY_CANDIDATE_PRIMITIVE) { fn = "rayQueryGetIntersectionPrimitiveIndexEXT"; committed = "false"; castUint = true; }
        else if (name == BUILTIN_RAY_QUERY_CANDIDATE_INSTANCE)  { fn = "rayQueryGetIntersectionInstanceIdEXT"; committed = "false"; castUint = true; }
        else if (name == BUILTIN_RAY_QUERY_CANDIDATE_BARYCENTRICS) { fn = "rayQueryGetIntersectionBarycentricsEXT"; committed = "false"; }
        else return false;
        if (castUint) out << "uint(";
        out << fn << "(";
        cg.generateExpr(_expr->args[0]);
        out << ", " << committed << ")";
        if (castUint) out << ")";
        return true;
    }

    bool GLSLTarget::tryEmitBuiltinCall(CodeGen &cg,
                                        ast::CallExpr *_expr,
                                        OmegaCommon::StrRef name,
                                        std::ostream &out) {
        /// Sub-phase 1.5 — low-level ray-query traversal family.
        if (glslEmitRayQuery(cg, _expr, name, out)) return true;
        /// §5 — `dispatchMesh(x, y, z, payload)` → `EmitMeshTasksEXT(x, y, z)`.
        /// A rewrite rather than a rename because the payload argument has no
        /// place at the GLSL call site: the payload is the file-scope
        /// `taskPayloadSharedEXT` global emitted in `emitShaderEntryHeader`, and
        /// `EmitMeshTasksEXT` picks it up implicitly. Sema has already proven
        /// the 4th argument names exactly that payload parameter, so dropping it
        /// here loses nothing.
        if (name == BUILTIN_DISPATCH_MESH) {
            if (_expr->args.size() != 4) return false;
            out << "EmitMeshTasksEXT(";
            for (unsigned i = 0; i < 3; i++) {
                if (i > 0) out << ", ";
                cg.generateExpr(_expr->args[i]);
            }
            out << ")";
            return true;
        }
        /// §5.3: GLSL's `bitCount` returns a signed iN even for a uint
        /// operand, so wrap it in a cast back to the operand type to honor
        /// countbits' operand-typed return contract. The operand resolved
        /// type is stamped by Sema (§5.3 dispatch).
        if (name == BUILTIN_COUNTBITS) {
            if (_expr->args.size() != 1) return false;
            auto *ty = cg.typeResolver->resolveTypeWithExpr(_expr->args[0]->resolvedType);
            const char *spelling = glslIntTypeSpelling(ty);
            if(!spelling) return false; // defensive; Sema guarantees integer.
            out << spelling << "(bitCount(";
            cg.generateExpr(_expr->args[0]);
            out << "))";
            return true;
        }
        /// §5.3 Phase B — firstbithigh / firstbitlow. GLSL is the reference
        /// for the normalized contract: `findMSB`/`findLSB` already return a
        /// signed int / ivec and `-1` when no bit is set. To make signed and
        /// unsigned operands agree on the raw-bit-pattern index, the operand
        /// is first cast to its unsigned spelling (findMSB(int) treats the
        /// sign bit specially; findMSB(uint) is the plain MSB index that
        /// matches HLSL/MSL). The signed result type is what Sema assigned.
        if (name == BUILTIN_FIRSTBITHIGH || name == BUILTIN_FIRSTBITLOW) {
            if (_expr->args.size() != 1) return false;
            auto *ty = cg.typeResolver->resolveTypeWithExpr(_expr->args[0]->resolvedType);
            bool isSigned; int arity;
            if(!cg.intOperandShape(ty, isSigned, arity)) return false;
            /// Unsigned spelling for the cast; the surrounding findMSB/findLSB
            /// returns the signed iN result directly.
            const char *uvec = glslIntTypeSpelling(
                isSigned ? (arity==1?ast::builtins::uint_type
                          : arity==2?ast::builtins::uint2_type
                          : arity==3?ast::builtins::uint3_type
                          : ast::builtins::uint4_type)
                         : ty);
            const char *fn = (name == BUILTIN_FIRSTBITHIGH) ? "findMSB" : "findLSB";
            out << fn << "(" << uvec << "(";
            cg.generateExpr(_expr->args[0]);
            out << "))";
            return true;
        }
        /// §5.3 Phase C — GLSL `bitfieldExtract`/`bitfieldInsert` are native
        /// (scalar + vector, signed + unsigned), but the offset/bits args
        /// must be `int` (GLSL signature), so wrap them in `int(...)`. The
        /// value/base/insert operands are cast to the operand-type spelling:
        /// GLSL is strict about uint-vs-int, and a bare numeric literal
        /// (`0xFFu` → `255`) emits as a signed int, so the cast keeps the
        /// `bitfieldInsert(uint,uint,int,int)` overload unambiguous.
        if (name == BUILTIN_BITFIELD_EXTRACT || name == BUILTIN_BITFIELD_INSERT) {
            bool isSigned; int arity;
            auto *ty = cg.typeResolver->resolveTypeWithExpr(_expr->args[0]->resolvedType);
            if (!cg.intOperandShape(ty, isSigned, arity)) return false;
            const char *vty = glslIntTypeSpelling(ty);
            if (!vty) return false;
            if (name == BUILTIN_BITFIELD_EXTRACT) {
                if (_expr->args.size() != 3) return false;
                out << "bitfieldExtract(" << vty << "("; cg.generateExpr(_expr->args[0]);
                out << "), int("; cg.generateExpr(_expr->args[1]);
                out << "), int("; cg.generateExpr(_expr->args[2]);
                out << "))";
            } else {
                if (_expr->args.size() != 4) return false;
                out << "bitfieldInsert(" << vty << "("; cg.generateExpr(_expr->args[0]);
                out << "), " << vty << "("; cg.generateExpr(_expr->args[1]);
                out << "), int("; cg.generateExpr(_expr->args[2]);
                out << "), int("; cg.generateExpr(_expr->args[3]);
                out << "))";
            }
            return true;
        }
        /// §5.5 Phase B — half-float pack / unpack. GLSL has the packed
        /// pair (`packHalf2x16` / `unpackHalf2x16`) native — those pass
        /// through the shared `(args)` print path without a rename. The
        /// scalar pair (`f16tof32` / `f32tof16`) has no GLSL equivalent
        /// and lowers inline through the packed forms:
        ///   f16tof32(x) → unpackHalf2x16(x).x — the high 16 bits become
        ///                 the discarded `.y` (no statement injection
        ///                 needed; `x` is referenced exactly once).
        ///   f32tof16(x) → packHalf2x16(vec2(x, 0.0)) — the high 16 bits
        ///                 are zero by construction.
        if (name == BUILTIN_F16TOF32) {
            if (_expr->args.size() != 1) return false;
            out << "unpackHalf2x16(";
            cg.generateExpr(_expr->args[0]);
            out << ").x";
            return true;
        }
        if (name == BUILTIN_F32TOF16) {
            if (_expr->args.size() != 1) return false;
            out << "packHalf2x16(vec2(";
            cg.generateExpr(_expr->args[0]);
            out << ", 0.0))";
            return true;
        }
        /// §5.5 Phase A — bit-pattern reinterpret. GLSL has named
        /// functions only for the float<->int / float<->uint directions
        /// (`floatBitsToInt`, `floatBitsToUint`, `intBitsToFloat`,
        /// `uintBitsToFloat`). int<->uint reinterpret uses a constructor
        /// cast (per the GLSL spec, `uint(i)` / `int(u)` preserves the
        /// bit pattern). Same-target identity emits the operand bare.
        if (name == BUILTIN_ASINT || name == BUILTIN_ASUINT || name == BUILTIN_ASFLOAT) {
            if (_expr->args.size() != 1) return false;
            using namespace ast::builtins;
            auto *ty = cg.typeResolver->resolveTypeWithExpr(_expr->args[0]->resolvedType);
            enum Kind { K_FLOAT, K_INT, K_UINT } src;
            int arity;
            if (ty == float_type)       { src = K_FLOAT; arity = 1; }
            else if (ty == float2_type) { src = K_FLOAT; arity = 2; }
            else if (ty == float3_type) { src = K_FLOAT; arity = 3; }
            else if (ty == float4_type) { src = K_FLOAT; arity = 4; }
            else if (ty == int_type)    { src = K_INT;   arity = 1; }
            else if (ty == int2_type)   { src = K_INT;   arity = 2; }
            else if (ty == int3_type)   { src = K_INT;   arity = 3; }
            else if (ty == int4_type)   { src = K_INT;   arity = 4; }
            else if (ty == uint_type)   { src = K_UINT;  arity = 1; }
            else if (ty == uint2_type)  { src = K_UINT;  arity = 2; }
            else if (ty == uint3_type)  { src = K_UINT;  arity = 3; }
            else if (ty == uint4_type)  { src = K_UINT;  arity = 4; }
            else return false; // defensive; Sema rejects upstream.
            Kind dst = (name == BUILTIN_ASINT)  ? K_INT
                     : (name == BUILTIN_ASUINT) ? K_UINT
                                                : K_FLOAT;
            if (src == dst) {
                /// Same-target identity. Emit the operand bare — GLSL
                /// has no `intBitsToInt` / `floatBitsToFloat`.
                cg.generateExpr(_expr->args[0]);
                return true;
            }
            const char *named = nullptr;
            if (src == K_FLOAT && dst == K_INT)  named = "floatBitsToInt";
            else if (src == K_FLOAT && dst == K_UINT) named = "floatBitsToUint";
            else if (src == K_INT && dst == K_FLOAT)  named = "intBitsToFloat";
            else if (src == K_UINT && dst == K_FLOAT) named = "uintBitsToFloat";
            if (named) {
                out << named << "(";
                cg.generateExpr(_expr->args[0]);
                out << ")";
                return true;
            }
            /// int <-> uint: constructor cast. Per the GLSL spec
            /// (4.1.3 "Integers"), int↔uint conversion preserves the
            /// bit pattern when the value is in range; out-of-range
            /// values reinterpret the bits, which is exactly the
            /// `asint` / `asuint` cross-sign semantics.
            const char *vty =
                (dst == K_INT)
                    ? (arity == 2 ? "ivec2" : arity == 3 ? "ivec3" : arity == 4 ? "ivec4" : "int")
                    : (arity == 2 ? "uvec2" : arity == 3 ? "uvec3" : arity == 4 ? "uvec4" : "uint");
            out << vty << "(";
            cg.generateExpr(_expr->args[0]);
            out << ")";
            return true;
        }
        /// §5.1: GLSL has no `saturate` — rewrite `saturate(x)` as
        /// `clamp(x, 0.0, 1.0)`. GLSL's `clamp(genType, float, float)`
        /// overload broadcasts the scalar bounds across vector x, so the
        /// same emission works for scalar and vector arguments.
        if (name == BUILTIN_SATURATE) {
            if (_expr->args.size() != 1) return false;
            out << "clamp(";
            cg.generateExpr(_expr->args[0]);
            out << ", 0.0, 1.0)";
            return true;
        }
        /// §5.1: GLSL's `mod` has floor-based semantics that disagree with
        /// C/HLSL/MSL `fmod` for negative inputs. Emit the C form directly
        /// using `trunc`: `(x - y * trunc(x / y))`. Vectorizes per-component.
        if (name == BUILTIN_FMOD) {
            if (_expr->args.size() != 2) return false;
            out << "(";
            cg.generateExpr(_expr->args[0]);
            out << " - ";
            cg.generateExpr(_expr->args[1]);
            out << " * trunc(";
            cg.generateExpr(_expr->args[0]);
            out << " / ";
            cg.generateExpr(_expr->args[1]);
            out << "))";
            return true;
        }
        /// §5.6 — atomic operations. GLSL carries atomicity on the operation
        /// (`atomic*` on a plain int/uint SSBO / shared slot). The fetch-ops /
        /// exchange return the original value inline; the operand is cast to
        /// the underlying type so a bare literal can't make the int/uint
        /// overload ambiguous. `atomic_load`/`atomic_store` are a plain read /
        /// write (32-bit aligned access is atomic).
        {
            const char *verb = nullptr;
            if (name == BUILTIN_ATOMIC_ADD)           verb = "atomicAdd";
            else if (name == BUILTIN_ATOMIC_MIN)      verb = "atomicMin";
            else if (name == BUILTIN_ATOMIC_MAX)      verb = "atomicMax";
            else if (name == BUILTIN_ATOMIC_AND)      verb = "atomicAnd";
            else if (name == BUILTIN_ATOMIC_OR)       verb = "atomicOr";
            else if (name == BUILTIN_ATOMIC_XOR)      verb = "atomicXor";
            else if (name == BUILTIN_ATOMIC_EXCHANGE) verb = "atomicExchange";
            if (verb) {
                auto *mty = cg.typeResolver->resolveTypeWithExpr(_expr->args[0]->resolvedType);
                const char *uty = (mty == ast::builtins::atomic_int_type) ? "int" : "uint";
                out << verb << "(";
                cg.generateExpr(_expr->args[0]);
                out << ", " << uty << "(";
                cg.generateExpr(_expr->args[1]);
                out << "))";
                return true;
            }
            if (name == BUILTIN_ATOMIC_LOAD) {
                cg.generateExpr(_expr->args[0]);
                return true;
            }
            if (name == BUILTIN_ATOMIC_STORE) {
                out << "(";
                cg.generateExpr(_expr->args[0]);
                out << " = ";
                cg.generateExpr(_expr->args[1]);
                out << ")";
                return true;
            }
            /// §5.6 Phase B — CAS. GLSL's `atomicCompSwap` is native strong and
            /// returns the original value inline; the compare/desired operands
            /// are cast to the underlying type for overload disambiguation.
            if (name == BUILTIN_ATOMIC_COMPARE_EXCHANGE) {
                auto *mty = cg.typeResolver->resolveTypeWithExpr(_expr->args[0]->resolvedType);
                const char *uty = (mty == ast::builtins::atomic_int_type) ? "int" : "uint";
                out << "atomicCompSwap(";
                cg.generateExpr(_expr->args[0]);
                out << ", " << uty << "(";
                cg.generateExpr(_expr->args[1]);
                out << "), " << uty << "(";
                cg.generateExpr(_expr->args[2]);
                out << "))";
                return true;
            }
            /// §5.6 Phase B — weak CAS. GLSL has no weak form; emulated from the
            /// strong `atomicCompSwap` (strong satisfies the weak contract):
            /// capture the original, `ok = (orig == expected)` against the OLD
            /// expected, then `expected = orig`. Value = the bool.
            if (name == BUILTIN_ATOMIC_COMPARE_EXCHANGE_WEAK) {
                auto *mty = cg.typeResolver->resolveTypeWithExpr(_expr->args[0]->resolvedType);
                const char *uty = (mty == ast::builtins::atomic_int_type) ? "int" : "uint";
                std::string mem = cg.renderExprToString(_expr->args[0]);
                std::string exp = cg.renderExprToString(_expr->args[1]);
                std::string des = cg.renderExprToString(_expr->args[2]);
                unsigned id = cg.getDimensionsTempId++;
                std::string o  = "_cw" + std::to_string(id) + "_o";
                std::string ok = "_cw" + std::to_string(id) + "_ok";
                cg.queuePendingStatement(std::string(uty) + " " + o + " = atomicCompSwap(" + mem
                    + ", " + std::string(uty) + "(" + exp + "), " + std::string(uty) + "(" + des + "));");
                cg.queuePendingStatement("bool " + ok + " = (" + o + " == " + exp + ");");
                cg.queuePendingStatement(exp + " = " + o + ";");
                out << ok;
                return true;
            }
        }
        return false;
    }

    void GLSLTarget::emitTextureRead(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        auto *texTy = glslResolveTextureType(cg, _expr->args[0]);
        const char *coordCast = glslIntCoordTypeForTexture(cg, _expr->args[0]);
        bool isMS = (texTy == ast::builtins::texture2d_ms_type
                     || texTy == ast::builtins::texture2d_ms_array_type);

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
        if(isMS){
            /// GLSL `texelFetch(textureNDMS, coord, sample_index)` — the
            /// trailing slot is the sample index, not a mip level. MS
            /// textures have only one mip.
            out << ",int(";
            cg.generateExpr(_expr->args[2]);
            out << "))";
        } else {
            out << ",0)";
        }
    }

    void GLSLTarget::emitTextureCalculateLOD(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// `textureQueryLod(samplerND(t, s), spatialCoord).x`. GLSL returns a
        /// vec2 (clamped, unclamped); `.x` is the clamped LOD, matching the
        /// advisory-LOD contract. `textureQueryLod` needs a real combined
        /// sampler (it is not in GL_EXT_samplerless_texture_functions). The
        /// array layer / cube-array face is dropped from the coord (`.xy` for
        /// 2D-array, `.xyz` for cube-array). 1D is rejected in Sema.
        auto samplerType = glslSamplerTypeForTextureArg(cg, _expr->args[1], _expr->args[0]);
        auto *texTy = glslResolveTextureType(cg, _expr->args[1]);
        out << "textureQueryLod(" << samplerType << "(";
        cg.generateExpr(_expr->args[1]);
        out << ",";
        cg.generateExpr(_expr->args[0]);
        out << "),";
        if(texTy == ast::builtins::texture2d_array_type){
            out << "("; cg.generateExpr(_expr->args[2]); out << ").xy";
        } else if(texTy == ast::builtins::texturecube_array_type){
            out << "("; cg.generateExpr(_expr->args[2]); out << ").xyz";
        } else {
            cg.generateExpr(_expr->args[2]);
        }
        out << ").x";
    }

    void GLSLTarget::emitTextureGetDimensions(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// `uvecN(textureSize(t, int(lod)))`, emitted inline. The bare texture
        /// object works via GL_EXT_samplerless_texture_functions (enabled in
        /// the preamble), so no combined sampler is needed. `textureSize`
        /// returns int / ivec2 / ivec3; the `uvecN` constructor casts to the
        /// uint shape Sema assigned.
        auto *texTy = glslResolveTextureType(cg, _expr->args[0]);
        const char *ctor = "uint";
        if(texTy == ast::builtins::texture1d_array_type
           || texTy == ast::builtins::texture2d_type
           || texTy == ast::builtins::texturecube_type){
            ctor = "uvec2";
        } else if(texTy == ast::builtins::texture2d_array_type
                  || texTy == ast::builtins::texture3d_type
                  || texTy == ast::builtins::texturecube_array_type){
            ctor = "uvec3";
        }
        out << ctor << "(textureSize(";
        cg.generateExpr(_expr->args[0]);
        out << ",int(";
        cg.generateExpr(_expr->args[1]);
        out << ")))";
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

    void GLSLTarget::emitIntersect(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// Inline ray tracing (Raytracing plan §2.2). `intersect(as, ray[,
        /// mask])` → `GL_EXT_ray_query`: initialize a `rayQueryEXT`, drain the
        /// traversal loop, then read the committed intersection into a `RayHit`.
        /// Statement-shaped, so queued as preceding statements and the
        /// expression is the injected `RayHit` temp (same pattern as the HLSL
        /// backend). `rayQueryInitializeEXT` takes origin/tmin/direction/tmax as
        /// separate arguments (not a RayDesc).
        std::string asStr = cg.renderExprToString(_expr->args[0]);
        std::string rayStr = cg.renderExprToString(_expr->args[1]);
        /// Missing mask ⇒ 0xFF. The GLSL cull mask is a `uint`; a literal like
        /// `0xFF` resolves to `int` in Sema, so wrap an explicit mask in uint().
        std::string maskStr = (_expr->args.size() == 3)
            ? ("uint(" + cg.renderExprToString(_expr->args[2]) + ")")
            : std::string("0xFFu");

        unsigned id = cg.rayQueryTempId++;
        std::string q = "_rq" + std::to_string(id);
        std::string h = "_rh" + std::to_string(id);

        cg.queuePendingStatement("rayQueryEXT " + q + ";");
        cg.queuePendingStatement("rayQueryInitializeEXT(" + q + ", " + asStr
            + ", gl_RayFlagsOpaqueEXT, " + maskStr + ", "
            + rayStr + ".origin, " + rayStr + ".tmin, "
            + rayStr + ".direction, " + rayStr + ".tmax);");
        cg.queuePendingStatement("while(rayQueryProceedEXT(" + q + ")) {}");
        cg.queuePendingStatement("RayHit " + h + ";");
        cg.queuePendingStatement(h + ".committed = (rayQueryGetIntersectionTypeEXT(" + q
            + ", true) == gl_RayQueryCommittedIntersectionTriangleEXT);");
        cg.queuePendingStatement(h + ".t = rayQueryGetIntersectionTEXT(" + q + ", true);");
        cg.queuePendingStatement(h + ".primitiveIndex = uint(rayQueryGetIntersectionPrimitiveIndexEXT(" + q + ", true));");
        cg.queuePendingStatement(h + ".instanceIndex = uint(rayQueryGetIntersectionInstanceIdEXT(" + q + ", true));");
        cg.queuePendingStatement(h + ".barycentrics = rayQueryGetIntersectionBarycentricsEXT(" + q + ", true);");

        out << h;
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
        /// `OutputCoverage` is intentionally absent here. `gl_SampleMask` is
        /// `int[]` while the OmegaSL field is `uint`, so direct lowering
        /// would produce a `int = uint` mismatch. Output-coverage writes
        /// flow through `writeInternalFieldRef` to a synthetic uint local
        /// declared at shader entry, and the int-cast write-back to
        /// `gl_SampleMask[0]` is emitted by `emitShaderEntryBody` /
        /// `tryEmitReturnDecl` before each return.
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
        /// §12.2 follow-up — integer matrices have no GLSL matrix type; spell
        /// the column vector (`ivecR`/`uvecR`). The declarator site appends
        /// the `[C]` array dimension via `CodeGen::writeDeclTypeSuffix`.
        {
            bool isSigned; unsigned cols, rows;
            if(CodeGen::integerMatrixShape(t, isSigned, cols, rows)){
                out << (isSigned ? "ivec" : "uvec") << rows;
                return;
            }
        }
        if(t == ast::builtins::void_type){
            out << "void";
        }
        else if(t == ast::builtins::bool_type){
            out << "bool";
        }
        else if(t == ast::builtins::bool2_type){ out << "bvec2"; }
        else if(t == ast::builtins::bool3_type){ out << "bvec3"; }
        else if(t == ast::builtins::bool4_type){ out << "bvec4"; }
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
        /// §5.6 — atomic scalars. GLSL carries atomicity on the operation
        /// (`atomic*` on a plain int/uint SSBO / shared slot), so the type
        /// is the underlying scalar.
        else if(t == ast::builtins::atomic_int_type){ out << "int"; }
        else if(t == ast::builtins::atomic_uint_type){ out << "uint"; }
        /// Inline ray tracing (Raytracing plan §2.2). Spelled when the TLAS
        /// handle is passed to a user helper; the resource declaration itself
        /// is emitted in `emitResourceBinding`. `Ray`/`RayHit` fall through to
        /// the default (`t->name`) — their names match the emitted structs.
        else if(t == ast::builtins::acceleration_structure_type){ out << "accelerationStructureEXT"; }
        /// Sub-phase 1.5 — the low-level ray-query object. Declared as a local
        /// (`rayQueryEXT q;`) and mutated by the `ray_query_*` intrinsics.
        else if(t == ast::builtins::ray_query_type){ out << "rayQueryEXT"; }
        /// §4.1 16-bit family. Spelt with the explicit-arithmetic-types
        /// names from `GL_EXT_shader_explicit_arithmetic_types_*`.
        /// `emitDefaultHeaders` adds the matching `#extension` lines
        /// when the file declares `#requires(FLOAT16)`.
        else if(t == ast::builtins::half_type)   { out << "float16_t"; }
        else if(t == ast::builtins::half2_type)  { out << "f16vec2"; }
        else if(t == ast::builtins::half3_type)  { out << "f16vec3"; }
        else if(t == ast::builtins::half4_type)  { out << "f16vec4"; }
        else if(t == ast::builtins::short_type)  { out << "int16_t"; }
        else if(t == ast::builtins::short2_type) { out << "i16vec2"; }
        else if(t == ast::builtins::short3_type) { out << "i16vec3"; }
        else if(t == ast::builtins::short4_type) { out << "i16vec4"; }
        else if(t == ast::builtins::ushort_type) { out << "uint16_t"; }
        else if(t == ast::builtins::ushort2_type){ out << "u16vec2"; }
        else if(t == ast::builtins::ushort3_type){ out << "u16vec3"; }
        else if(t == ast::builtins::ushort4_type){ out << "u16vec4"; }
        /// §4.2 64-bit ints — same extension-driven pattern.
        else if(t == ast::builtins::long_type)   { out << "int64_t"; }
        else if(t == ast::builtins::long2_type)  { out << "i64vec2"; }
        else if(t == ast::builtins::long3_type)  { out << "i64vec3"; }
        else if(t == ast::builtins::long4_type)  { out << "i64vec4"; }
        else if(t == ast::builtins::ulong_type)  { out << "uint64_t"; }
        else if(t == ast::builtins::ulong2_type) { out << "u64vec2"; }
        else if(t == ast::builtins::ulong3_type) { out << "u64vec3"; }
        else if(t == ast::builtins::ulong4_type) { out << "u64vec4"; }
        /// §4.3 double-precision floats. GLSL spells the scalar `double`
        /// and the vectors `dvecN`; `emitDefaultHeaders` adds the
        /// `GL_ARB_gpu_shader_fp64` extension when the file
        /// `#requires(DOUBLE)`.
        else if(t == ast::builtins::double_type)  { out << "double"; }
        else if(t == ast::builtins::double2_type) { out << "dvec2"; }
        else if(t == ast::builtins::double3_type) { out << "dvec3"; }
        else if(t == ast::builtins::double4_type) { out << "dvec4"; }
        else {
            out << t->name;
        }

        if(pointer){
            out << " * ";
        }
    }

}
