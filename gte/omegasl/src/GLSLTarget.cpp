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
        }
        return "";
    }

    bool GLSLTarget::compileShader(ast::ShaderDecl::Type stage,
                                   OmegaCommon::StrRef name,
                                   uint64_t /*requiredFeatures*/,
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
            case ast::ShaderDecl::Hull:
            case ast::ShaderDecl::Domain:   break;
        }

        auto options = shaderc_compile_options_initialize();

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

    /// Curated set of GLSL builtin functions that would shadow a user
    /// function with the same name. Mangled with `osl_user_<name>` when
    /// hit. The reserved-keyword set (input/output/shared/etc.) is
    /// handled separately by writeGLSLIdent for variable identifiers.
    bool GLSLTarget::needsMangling(OmegaCommon::StrRef name) const {
        static const std::unordered_set<std::string> glslStdlib = {
            "abs","acos","asin","atan","ceil","clamp","cos","cosh","cross",
            "degrees","determinant","distance","dot","dFdx","dFdy","exp",
            "exp2","floor","fract","fwidth","inverse","inversesqrt","length",
            "log","log2","max","min","mix","mod","normalize","pow","radians",
            "reflect","refract","round","sign","sin","sinh","smoothstep",
            "sqrt","step","tan","tanh","transpose","trunc",
            "texture","textureLod","textureGrad","texelFetch","imageStore",
            "imageLoad"
        };
        return glslStdlib.count(std::string(name)) > 0;
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

    void GLSLTarget::emitDefaultHeaders(CodeGen &cg, std::ostream &out) {
        out << "// Warning! This file has been generated by omegaslc! DO NOT EDIT!!\n"
               "#version 450\n"
               "#extension GL_EXT_samplerless_texture_functions : enable" << std::endl;

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
    }

    void GLSLTarget::emitStructDecl(CodeGen &cg, ast::StructDecl *_decl) {
        std::ostringstream out;
        if (_decl->internal) {
            internalStructs.push_back(_decl);
        } else {
            out << "struct " << _decl->name << " {" << std::endl;
            for (auto &f : _decl->fields) {
                out << "  ";
                cg.writeTypeExpr(f.typeExpr, out);
                out << " " << f.name << ";" << std::endl;
            }
            out << "};";
        }
        generatedStructs.insert(std::make_pair(_decl->name, out.str()));
        structDeclMap[_decl->name] = _decl;
    }

    /// GLSL emits its used-struct text inside `emitShaderEntryHeader`
    /// (interleaved with the fragment-output / interstage varying decls
    /// for internal structs), so the pre-entry-header pass is a no-op.
    void GLSLTarget::emitShaderUsedStructs(CodeGen &/*cg*/, ast::ShaderDecl */*decl*/,
                                           std::ostream &/*out*/) {}

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
            for (unsigned dim : _var->typeExpr->arrayDims) {
                out << "[" << dim << "]";
            }
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

        shader_entry.type = _decl->shaderType == ast::ShaderDecl::Vertex   ? OMEGASL_SHADER_VERTEX
                          : _decl->shaderType == ast::ShaderDecl::Fragment ? OMEGASL_SHADER_FRAGMENT
                          : _decl->shaderType == ast::ShaderDecl::Compute  ? OMEGASL_SHADER_COMPUTE
                          : _decl->shaderType == ast::ShaderDecl::Hull     ? OMEGASL_SHADER_HULL
                                                                            : OMEGASL_SHADER_DOMAIN;
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
                OmegaCommon::String mode =
                    (_decl->shaderType == ast::ShaderDecl::Fragment) ? "in" : "out";
                unsigned idx = 0;
                for (auto &f : _struct->fields) {
                    if (f.attributeName.value() != ATTRIBUTE_POSITION) {
                        out << "layout(location =" << idx << ") " << mode << " ";
                        writeTypeName(cg.typeResolver->resolveTypeWithExpr(f.typeExpr),
                                      f.typeExpr->pointer, out);
                        out << " " << _struct->name << "_" << f.name << ";" << std::endl;
                    }
                    idx += 1;
                }
            } else {
                out << generatedStructs[s] << std::endl << std::endl;
            }
        }

        extra_stmts.str("");
        extra_stmts.clear();

        cg.indentLevel += 1;

        if (_decl->shaderType == ast::ShaderDecl::Fragment) {
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

        /// Resource bindings (file scope for GLSL).
        cg.emitResourcesAndFillLayout(_decl, shader_entry, out);

        /// Standard shader arguments. Compute uses `layout(local_size_*)`
        /// + attribute-bridge locals; vertex/hull/domain use
        /// `layout(location=N) in` + attribute-bridge for VertexID;
        /// fragment scalar inputs bridge from `gl_*` builtins; fragment
        /// struct inputs land in `internalStructVarMap` for member
        /// rerouting at body emission time.
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
                    }
                } else {
                    out << "layout(location = ";
                    out << arg_idx << ") in ";
                    writeTypeName(cg.typeResolver->resolveTypeWithExpr(arg.typeExpr),
                                  arg.typeExpr->pointer, out);
                    out << " " << arg.name << " ;" << std::endl;
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

        /// Custom body loop with RETURN_DECL rerouting for fragment-output
        /// struct returns and hull/domain `gl_Position` writes.
        for (auto stmt_it = _decl->block->body.begin(); stmt_it != _decl->block->body.end(); stmt_it++) {
            auto stmt = *stmt_it;
            /// §6.1 — `threadgroup` decls are hoisted to file scope by
            /// `emitThreadgroupGlobals` as `shared`; skip them in the body.
            if (stmt->type == VAR_DECL && ((ast::VarDecl *)stmt)->isThreadgroup) {
                continue;
            }
            for (unsigned i = 0; i < cg.indentLevel; i++) {
                out << "    ";
            }
            if (stmt->type == RETURN_DECL) {
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
                    /// Hull/Domain: main() is void in GLSL — assign Position
                    /// field to gl_Position output.
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
            } else if ((stmt->type & DECL) != EXPR) {
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

        /// Reset per-shader state — must happen after body emission so
        /// MEMBER_EXPR rerouting still works during the body.
        internalStructVarMap.clear();
        activeReturnReplacement.clear();
        fragmentOutputStruct = nullptr;
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
            out << "gl_Position";
        } else if (attr == ATTRIBUTE_COLOR && field.attributeIndex.has_value()) {
            out << "_outColor" << field.attributeIndex.value();
        } else if (attr == ATTRIBUTE_DEPTH) {
            out << "gl_FragDepth";
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
        /// Default emission: same as the abstract Target default.
        cg.generateExpr(expr->lhs);
        out << "." << expr->rhs_id;
    }

    void GLSLTarget::emitResourceBinding(CodeGen &cg,
                                         ast::ResourceDecl *res_decl,
                                         ast::ShaderDecl *shader,
                                         omegasl_shader_layout_desc_io_mode ioMode,
                                         std::ostream &out,
                                         omegasl_shader_layout_desc &layoutDesc) {
        unsigned set = (shader->shaderType == ast::ShaderDecl::Fragment) ? 1 : 0;
        auto type_ = cg.typeResolver->resolveTypeWithExpr(res_decl->typeExpr);

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
        layoutDesc.gpu_relative_loc = binding;
        layoutDesc.offset = 0;
        layoutDesc.io_mode = ioMode;
        if (res_decl->isStatic) {
            layoutDesc.sampler_desc.max_anisotropy = res_decl->staticSamplerDesc->maxAnisotropy;
            layoutDesc.sampler_desc.filter = res_decl->staticSamplerDesc->filter;
            layoutDesc.sampler_desc.u_address_mode = res_decl->staticSamplerDesc->uAddressMode;
            layoutDesc.sampler_desc.v_address_mode = res_decl->staticSamplerDesc->vAddressMode;
            layoutDesc.sampler_desc.w_address_mode = res_decl->staticSamplerDesc->wAddressMode;
        }

        ++binding;
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
        /// §6.2 — compute barriers. `barrier()` is the execution +
        /// shared-memory control barrier; `memoryBarrier()` is the
        /// device-memory-only ordering barrier (no execution sync).
        if (name == BUILTIN_THREADGROUP_BARRIER) return "barrier";
        if (name == BUILTIN_DEVICE_BARRIER)      return "memoryBarrier";
        if (name == BUILTIN_MAKE_FLOAT2)   return "vec2";
        if (name == BUILTIN_MAKE_FLOAT3)   return "vec3";
        if (name == BUILTIN_MAKE_FLOAT4)   return "vec4";
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

    bool GLSLTarget::tryEmitBuiltinCall(CodeGen &cg,
                                        ast::CallExpr *_expr,
                                        OmegaCommon::StrRef name,
                                        std::ostream &out) {
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
        else {
            out << t->name;
        }

        if(pointer){
            out << " * ";
        }
    }

}
