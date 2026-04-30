#include "Target.h"
#include "AST.h"
#include "CodeGen.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ostream>
#include <sstream>
#include <unordered_set>
#include <string>

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
                                   const OmegaCommon::FS::Path &srcDir,
                                   const OmegaCommon::FS::Path &outDir) {
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
                /// `Color(N)` field. `Depth` rides on `gl_FragDepth`,
                /// `Coverage` on `gl_SampleMask` — neither needs a decl.
                for (auto &f : fragmentOutputStruct->fields) {
                    if (!f.attributeName.has_value()) continue;
                    if (f.attributeName.value() == ATTRIBUTE_COLOR
                        && f.attributeIndex.has_value()) {
                        unsigned loc = f.attributeIndex.value();
                        out << "layout(location=" << loc << ") out vec4 _outColor"
                            << loc << ";" << std::endl;
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
                    } else if (arg.attributeName.value() == ATTRIBUTE_COVERAGE) {
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
            for (unsigned i = 0; i < cg.indentLevel; i++) {
                out << "    ";
            }
            if (stmt->type == RETURN_DECL) {
                auto _return_stmt = (ast::ReturnDecl *)stmt;
                if (_return_stmt->expr && _decl->shaderType == ast::ShaderDecl::Fragment
                    && fragmentOutputStruct != nullptr) {
                    /// Fragment-output-struct return: per-field stores
                    /// happened earlier via member-expr routing.
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
                if (stmt->type != IF_STMT && stmt->type != FOR_STMT && stmt->type != WHILE_STMT) {
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
        } else if (attr == ATTRIBUTE_COVERAGE) {
            /// gl_SampleMask is an int[]; index 0 covers up to 32-sample MSAA.
            out << "gl_SampleMask[0]";
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
        if (name == BUILTIN_MAKE_FLOAT2)   return "vec2";
        if (name == BUILTIN_MAKE_FLOAT3)   return "vec3";
        if (name == BUILTIN_MAKE_FLOAT4)   return "vec4";
        if (name == BUILTIN_MAKE_INT2)     return "ivec2";
        if (name == BUILTIN_MAKE_INT3)     return "ivec3";
        if (name == BUILTIN_MAKE_INT4)     return "ivec4";
        if (name == BUILTIN_MAKE_UINT2)    return "uvec2";
        if (name == BUILTIN_MAKE_UINT3)    return "uvec3";
        if (name == BUILTIN_MAKE_UINT4)    return "uvec4";
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
