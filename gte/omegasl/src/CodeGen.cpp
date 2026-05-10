#include "CodeGen.h"
#include <iomanip>
#include <sstream>

namespace omegasl {

    /// Print a float literal so it cannot be misread as an integer literal.
    /// `ostream << float` drops a trailing `.0`, so a source `0.0` becomes
    /// `0` in the generated shader. Every backend has integer overloads
    /// for `max` / `min` / `clamp` / etc. — an integer-shaped argument
    /// either picks the int overload silently (HLSL/MSL) or hard errors
    /// (GLSL). Centralized here so the three backends share one rule.
    static std::string formatFloatLit(double v) {
        std::ostringstream os;
        os << std::setprecision(9) << v;
        std::string s = os.str();
        if (s.find_first_of(".eEnN") == std::string::npos) {
            s += ".0";
        }
        return s;
    }

    /// Default member-access emission used by HLSL/MSL: walk the LHS,
    /// then write `.<rhs_id>`. GLSLTarget overrides this to consult
    /// the fragment-output struct rerouting map first.
    void Target::emitMemberExpr(CodeGen &cg, ast::MemberExpr *expr, std::ostream &out) {
        cg.generateExpr(expr->lhs);
        out << "." << expr->rhs_id;
    }

    /// Default index-expression emission used by GLSL/MSL: walk the LHS,
    /// then `[idx]`. HLSLTarget overrides to swap matrix indices for
    /// source-level alignment with OmegaSL's column-first convention.
    void Target::emitIndexExpr(CodeGen &cg, ast::IndexExpr *expr, std::ostream &out) {
        cg.generateExpr(expr->lhs);
        out << "[";
        cg.generateExpr(expr->idx_expr);
        out << "]";
    }

    /// Default shader-entry header: no-op. HLSL/GLSL override; MSL keeps
    /// its inline emission in `MetalCodeGen::SHADER_DECL` (its header
    /// interleaves with resource binding and closes with a manual `){`
    /// that we can't cleanly delegate yet — collapsed in Phase 8d).
    void Target::emitShaderEntryHeader(CodeGen &/*cg*/,
                                       ast::ShaderDecl */*decl*/,
                                       omegasl_shader &/*meta*/,
                                       std::ostream &/*out*/) {
    }

    /// Default shader-entry body: just walk the user's block. HLSL and
    /// MSL inherit this. GLSL overrides because its body needs custom
    /// RETURN_DECL rerouting for fragment-output struct returns and
    /// hull/domain `gl_Position` writes.
    void Target::emitShaderEntryBody(CodeGen &cg,
                                     ast::ShaderDecl *decl,
                                     omegasl_shader &/*meta*/,
                                     std::ostream &/*out*/) {
        cg.generateBlock(*decl->block);
    }

    /// Default footer: nothing. GLSL/HLSL/MSL inherit this for now.
    void Target::emitShaderEntryFooter(CodeGen &/*cg*/,
                                       ast::ShaderDecl */*decl*/,
                                       std::ostream &/*out*/) {
    }

    /// Shared expression-AST walk. Promoted to non-virtual on `CodeGen`
    /// after Phase 7.5 + 8a unified the per-backend bodies. Per-target
    /// divergence flows through `Target::*` hooks.
    void CodeGen::generateExpr(ast::Expr *expr) {
        std::ostream &out = shaderOutStream();
        switch (expr->type) {
            case BINARY_EXPR: {
                auto _expr = (ast::BinaryExpr *)expr;
                out << "(";
                generateExpr(_expr->lhs);
                out << " " << _expr->op << " ";
                generateExpr(_expr->rhs);
                out << ")";
                break;
            }
            case TERNARY_EXPR: {
                /// §3.2 — `(cond ? then : else)`. The spelling is the
                /// same on every backend Sema-side because we restrict
                /// the condition to a scalar `bool`. Outer parens keep
                /// `a + (cond ? b : c) * d` from miscoupling against
                /// surrounding precedence.
                auto _expr = (ast::TernaryExpr *)expr;
                out << "(";
                generateExpr(_expr->condition);
                out << " ? ";
                generateExpr(_expr->thenExpr);
                out << " : ";
                generateExpr(_expr->elseExpr);
                out << ")";
                break;
            }
            case LITERAL_EXPR: {
                auto _expr = (ast::LiteralExpr *)expr;
                if (_expr->isFloat()) {
                    out << formatFloatLit(_expr->f_num.value());
                } else if (_expr->isDouble()) {
                    out << formatFloatLit(_expr->d_num.value());
                } else if (_expr->isInt()) {
                    out << _expr->i_num.value();
                } else if (_expr->isUint()) {
                    out << _expr->ui_num.value();
                } else if (_expr->isLong()) {
                    /// §4.2 — emit a backend-portable 64-bit signed
                    /// suffix. HLSL/GLSL with the int64 extensions and
                    /// MSL all accept `123L`.
                    out << _expr->i64_num.value() << "L";
                } else if (_expr->isUlong()) {
                    out << _expr->ui64_num.value() << "UL";
                } else if (_expr->isBool()) {
                    out << (_expr->b_val.value() ? "true" : "false");
                } else if (_expr->isString()) {
                    out << _expr->str.value();
                }
                break;
            }
            case ID_EXPR: {
                auto _expr = (ast::IdExpr *)expr;
                target->writeIdentifier(_expr->id, out);
                break;
            }
            case MEMBER_EXPR: {
                auto _expr = (ast::MemberExpr *)expr;
                target->emitMemberExpr(*this, _expr, out);
                break;
            }
            case INDEX_EXPR: {
                auto _expr = (ast::IndexExpr *)expr;
                target->emitIndexExpr(*this, _expr, out);
                break;
            }
            case UNARY_EXPR: {
                auto _expr = (ast::UnaryOpExpr *)expr;
                if (_expr->isPrefix) {
                    out << _expr->op;
                    generateExpr(_expr->expr);
                } else {
                    generateExpr(_expr->expr);
                    out << _expr->op;
                }
                break;
            }
            case POINTER_EXPR: {
                auto _expr = (ast::PointerExpr *)expr;
                if (_expr->ptType == ast::PointerExpr::AddressOf) {
                    out << "&";
                } else {
                    out << "*";
                }
                generateExpr(_expr->expr);
                break;
            }
            case CALL_EXPR: {
                auto _expr = (ast::CallExpr *)expr;
                OmegaCommon::StrRef _id_expr = ((ast::IdExpr *)_expr->callee)->id;
                bool generatedExprBody = false;
                if (_id_expr == BUILTIN_SAMPLE) {
                    generatedExprBody = true;
                    target->emitTextureSample(*this, _expr, out);
                } else if (_id_expr == BUILTIN_SAMPLE_LOD) {
                    generatedExprBody = true;
                    target->emitTextureSampleLOD(*this, _expr, out);
                } else if (_id_expr == BUILTIN_SAMPLE_BIAS) {
                    generatedExprBody = true;
                    target->emitTextureSampleBias(*this, _expr, out);
                } else if (_id_expr == BUILTIN_SAMPLE_GRAD) {
                    generatedExprBody = true;
                    target->emitTextureSampleGrad(*this, _expr, out);
                } else if (_id_expr == BUILTIN_GATHER) {
                    generatedExprBody = true;
                    target->emitTextureGather(*this, _expr, /*channel=*/-1, out);
                } else if (_id_expr == BUILTIN_GATHER_RED) {
                    generatedExprBody = true;
                    target->emitTextureGather(*this, _expr, /*channel=*/0, out);
                } else if (_id_expr == BUILTIN_GATHER_GREEN) {
                    generatedExprBody = true;
                    target->emitTextureGather(*this, _expr, /*channel=*/1, out);
                } else if (_id_expr == BUILTIN_GATHER_BLUE) {
                    generatedExprBody = true;
                    target->emitTextureGather(*this, _expr, /*channel=*/2, out);
                } else if (_id_expr == BUILTIN_GATHER_ALPHA) {
                    generatedExprBody = true;
                    target->emitTextureGather(*this, _expr, /*channel=*/3, out);
                } else if (_id_expr == BUILTIN_WRITE) {
                    generatedExprBody = true;
                    target->emitTextureWrite(*this, _expr, out);
                } else if (_id_expr == BUILTIN_READ) {
                    generatedExprBody = true;
                    target->emitTextureRead(*this, _expr, out);
                } else if (isUserFunc(_id_expr)) {
                    /// §3.5 — pick the mangled spelling matching the
                    /// resolved overload. Sema stamps `resolvedCallee`
                    /// at CALL_EXPR resolution time; if it's missing
                    /// we fall back to the bare name (which still
                    /// works for the single-overload case).
                    if (_expr->resolvedCallee != nullptr) {
                        out << spellUserFuncName(_id_expr,
                                                 _expr->resolvedCallee->paramTypes);
                    } else {
                        out << spellUserFuncName(_id_expr);
                    }
                } else if (target->tryEmitBuiltinCall(*this, _expr, _id_expr, out)) {
                    /// GLSL handles `saturate` / `fmod` here — the call is
                    /// fully emitted by the backend (different shape, not a
                    /// simple rename), so skip the shared `(args)` suffix.
                    generatedExprBody = true;
                } else {
                    auto renamed = target->renameBuiltin(_id_expr);
                    out << renamed;
                }

                if (!generatedExprBody) {
                    out << "(";
                    for (auto e_it = _expr->args.begin(); e_it != _expr->args.end(); e_it++) {
                        if (e_it != _expr->args.begin()) {
                            out << ", ";
                        }
                        generateExpr(*e_it);
                    }
                    out << ")";
                }
                break;
            }
            case CAST_EXPR: {
                auto _expr = (ast::CastExpr *)expr;
                target->writeCast(*this, _expr->targetType, out);
                out << "(";
                generateExpr(_expr->expr);
                out << ")";
                break;
            }
            case ARRAY_EXPR: {
                auto _expr = (ast::ArrayExpr *)expr;
                out << "{";
                for (size_t i = 0; i < _expr->elm.size(); ++i) {
                    if (i != 0) {
                        out << ", ";
                    }
                    generateExpr(_expr->elm[i]);
                }
                out << "}";
                break;
            }
        }
    }

    /// Shared block-AST walk. Promoted to non-virtual on `CodeGen`
    /// after Phase 8c+8d unified the per-backend bodies. Output stream
    /// is fetched via `shaderOutStream()` until Phase 10 folds the
    /// file/string members up here too.
    void CodeGen::generateBlock(ast::Block &block) {
        std::ostream &out = shaderOutStream();
        out << "{" << std::endl;
        indentLevel += 1;
        for (auto stmt : block.body) {
            for (unsigned i = 0; i < indentLevel; i++) {
                out << "    ";
            }
            if (stmt->type == VAR_DECL || stmt->type == RETURN_DECL || stmt->type == IF_STMT
                || stmt->type == FOR_STMT || stmt->type == WHILE_STMT || stmt->type == BREAK_STMT
                || stmt->type == CONTINUE_STMT || stmt->type == DISCARD_STMT
                || stmt->type == SWITCH_STMT) {
                generateDecl((ast::Decl *)stmt);
                if (stmt->type != IF_STMT && stmt->type != FOR_STMT && stmt->type != WHILE_STMT
                    && stmt->type != SWITCH_STMT) {
                    out << ";";
                }
            } else {
                generateExpr((ast::Expr *)stmt);
                out << ";";
            }
            out << std::endl;
        }
        indentLevel -= 1;
        out << "}" << std::endl;
    }

    void CodeGen::emitResourcesAndFillLayout(ast::ShaderDecl *decl,
                                             omegasl_shader &meta,
                                             std::ostream &out) {
        OmegaCommon::Vector<omegasl_shader_layout_desc> layout;
        target->resetForNextShader();
        for (auto &res : decl->resourceMap) {
            auto &res_desc = *(resourceStore.find(res.name));
            omegasl_shader_layout_desc layoutDesc{};
            omegasl_shader_layout_desc_io_mode ioMode =
                res.access == ast::ShaderDecl::ResourceMapDesc::In    ? OMEGASL_SHADER_DESC_IO_IN
                : res.access == ast::ShaderDecl::ResourceMapDesc::Inout ? OMEGASL_SHADER_DESC_IO_INOUT
                                                                        : OMEGASL_SHADER_DESC_IO_OUT;
            target->emitResourceBinding(*this, res_desc, decl, ioMode, out, layoutDesc);

            /// Texture-swizzle plan §A.5 / Open Q1: reject swizzle on
            /// textures used as `out` / `inout` in this shader. All three
            /// runtime backends apply view-level swizzle to reads only —
            /// silently dropping it on writes would surprise compute-shader
            /// authors. The check lives here, not in Sema, because the
            /// read/write status is determined per-shader from `res.access`.
            if(res_desc->swizzleDesc.has_value()
               && ioMode != OMEGASL_SHADER_DESC_IO_IN){
                std::cerr << "error: `swizzle` cannot be applied to texture `"
                          << res_desc->name << "` because shader `" << decl->name
                          << "` uses it for write access. View-level swizzle is read-only on every backend; "
                             "use a shader-side swizzle (e.g. `output.bgra = value.rgba;`) instead." << std::endl;
                hasFatalErrors = true;
            }
            else if(res_desc->swizzleDesc.has_value()){
                /// Encoding 0=Identity, 1=R, 2=G, 3=B, 4=A, 5=Zero, 6=One.
                /// Storage layout is identical between AST and runtime
                /// `omegasl_texture_swizzle_desc`, so the assignment is a
                /// straight field copy.
                layoutDesc.swizzle_desc.r = res_desc->swizzleDesc->r;
                layoutDesc.swizzle_desc.g = res_desc->swizzleDesc->g;
                layoutDesc.swizzle_desc.b = res_desc->swizzleDesc->b;
                layoutDesc.swizzle_desc.a = res_desc->swizzleDesc->a;
            }

            layout.push_back(layoutDesc);
        }
        target->emitStaticPreamble(out);
        meta.nLayout = layout.size();
        if (!layout.empty()) {
            meta.pLayout = new omegasl_shader_layout_desc[layout.size()];
            std::copy(layout.begin(), layout.end(), meta.pLayout);
        }
    }

    void CodeGen::writeTypeExpr(ast::TypeExpr *typeExpr, std::ostream &out) {
        target->writeTypeName(typeResolver->resolveTypeWithExpr(typeExpr),
                              typeExpr->pointer, out);
    }

    void CodeGen::emitUserFunctionSignature(ast::FuncDecl *f) {
        writeTypeExpr(f->returnType, shaderOut);
        /// §3.5 — overloaded names mangle with a param-type suffix so
        /// each overload emits a distinct symbol. Build the positional
        /// type list from the FuncDecl's params (which preserves
        /// declaration order, unlike FuncType::fields).
        OmegaCommon::Vector<ast::TypeExpr *> paramTypes;
        paramTypes.reserve(f->params.size());
        for (auto &p : f->params) paramTypes.push_back(p.typeExpr);
        shaderOut << " " << spellUserFuncName(f->name, paramTypes) << "(";
        for (size_t i = 0; i < f->params.size(); i++) {
            if (i > 0) shaderOut << ", ";
            /// §3.7 — per-target spelling. HLSL/GLSL prefix the type with
            /// `out` / `inout`; MSL has no such keywords and rewrites the
            /// parameter as a `thread T&` reference.
            target->writeFuncParam(*this, f->params[i], shaderOut);
        }
        shaderOut << ")";
    }

    void CodeGen::emitUserFunctionPrototype(ast::FuncDecl *f) {
        emitUserFunctionSignature(f);
        shaderOut << ";" << std::endl;
    }

    void CodeGen::emitUserFunction(ast::FuncDecl *f) {
        emitUserFunctionSignature(f);
        shaderOut << std::endl;
        generateBlock(*f->block);
        shaderOut << std::endl;
    }

    /// Phase 10: shared, non-virtual decl walk. Per-target divergence
    /// for STRUCT_DECL / VAR_DECL / RETURN_DECL / SHADER_DECL flows
    /// through `Target::*` hooks. The if/for/while/break/continue/
    /// discard arms are byte-identical to the pre-Phase-10 backends.
    void CodeGen::generateDecl(ast::Decl *decl) {
        switch (decl->type) {
            case VAR_DECL: {
                auto _decl = (ast::VarDecl *)decl;
                if (target->tryEmitVarDecl(*this, _decl)) {
                    break;
                }
                /// §3.6 — `const` qualifier prefix. All three backends
                /// (HLSL, MSL, GLSL) accept the C-style `const T name = …;`
                /// form on a local declaration, so a single emit point
                /// before the type is enough — no per-target hook needed.
                if (_decl->isConst) {
                    shaderOut << "const ";
                }
                writeTypeExpr(_decl->typeExpr, shaderOut);
                shaderOut << " " << _decl->spec.name;
                if (_decl->typeExpr->arraySize.has_value()) {
                    shaderOut << "[" << _decl->typeExpr->arraySize.value() << "]";
                }
                if (_decl->spec.initializer.has_value()) {
                    shaderOut << " = ";
                    generateExpr(_decl->spec.initializer.value());
                }
                break;
            }
            case RETURN_DECL: {
                auto _decl = (ast::ReturnDecl *)decl;
                if (target->tryEmitReturnDecl(*this, _decl)) {
                    break;
                }
                if (_decl->expr) {
                    shaderOut << "return ";
                    generateExpr(_decl->expr);
                } else {
                    shaderOut << "return";
                }
                break;
            }
            case IF_STMT: {
                auto _stmt = (ast::IfStmt *)decl;
                shaderOut << "if(";
                generateExpr(_stmt->condition);
                shaderOut << ")";
                generateBlock(*_stmt->thenBlock);
                for (auto &branch : _stmt->elseIfs) {
                    shaderOut << " else if(";
                    generateExpr(branch.condition);
                    shaderOut << ")";
                    generateBlock(*branch.block);
                }
                if (_stmt->elseBlock) {
                    shaderOut << " else ";
                    generateBlock(*_stmt->elseBlock);
                }
                break;
            }
            case FOR_STMT: {
                auto _stmt = (ast::ForStmt *)decl;
                shaderOut << "for(";
                if (_stmt->init) { generateDecl((ast::Decl *)_stmt->init); }
                shaderOut << ";";
                if (_stmt->condition) { generateExpr(_stmt->condition); }
                shaderOut << ";";
                if (_stmt->increment) { generateExpr(_stmt->increment); }
                shaderOut << ")";
                generateBlock(*_stmt->body);
                break;
            }
            case WHILE_STMT: {
                auto _stmt = (ast::WhileStmt *)decl;
                shaderOut << "while(";
                generateExpr(_stmt->condition);
                shaderOut << ")";
                generateBlock(*_stmt->body);
                break;
            }
            case SWITCH_STMT: {
                /// C-style switch with fall-through. HLSL/MSL/GLSL all
                /// emit identical syntax, so this lives in shared codegen
                /// rather than per-target hooks. `break` inside a case
                /// flows through the existing BREAK_STMT arm.
                auto _stmt = (ast::SwitchStmt *)decl;
                shaderOut << "switch(";
                generateExpr(_stmt->condition);
                shaderOut << "){" << std::endl;
                for (auto &sc : _stmt->cases) {
                    for (unsigned i = 0; i < indentLevel; i++) shaderOut << "    ";
                    if (sc.value) {
                        shaderOut << "case ";
                        generateExpr(sc.value);
                        shaderOut << ":";
                    } else {
                        shaderOut << "default:";
                    }
                    shaderOut << std::endl;
                    indentLevel += 1;
                    for (auto *s : sc.body) {
                        for (unsigned i = 0; i < indentLevel; i++) shaderOut << "    ";
                        if (s->type == VAR_DECL || s->type == RETURN_DECL || s->type == IF_STMT
                            || s->type == FOR_STMT || s->type == WHILE_STMT || s->type == BREAK_STMT
                            || s->type == CONTINUE_STMT || s->type == DISCARD_STMT
                            || s->type == SWITCH_STMT) {
                            generateDecl((ast::Decl *)s);
                            if (s->type != IF_STMT && s->type != FOR_STMT && s->type != WHILE_STMT
                                && s->type != SWITCH_STMT) {
                                shaderOut << ";";
                            }
                        } else {
                            generateExpr((ast::Expr *)s);
                            shaderOut << ";";
                        }
                        shaderOut << std::endl;
                    }
                    indentLevel -= 1;
                }
                for (unsigned i = 0; i < indentLevel; i++) shaderOut << "    ";
                shaderOut << "}";
                break;
            }
            case BREAK_STMT: {
                shaderOut << "break";
                break;
            }
            case CONTINUE_STMT: {
                shaderOut << "continue";
                break;
            }
            case DISCARD_STMT: {
                auto kw = target->discardStatement();
                shaderOut << kw;
                break;
            }
            case STRUCT_DECL: {
                target->emitStructDecl(*this, (ast::StructDecl *)decl);
                break;
            }
            case RESOURCE_DECL: {
                resourceStore.add((ast::ResourceDecl *)decl);
                break;
            }
            case FUNC_DECL: {
                auto *_fd = (ast::FuncDecl *)decl;
                userFuncDecls.push_back(_fd);
                userFuncNames.insert(std::string(_fd->name));
                break;
            }
            case SHADER_DECL: {
                auto _decl = (ast::ShaderDecl *)decl;
                /// Record the AST pointer so the post-parse portability
                /// scanner (Feature-Gap-Survey §14.2) can revisit every
                /// shader after the full file has been parsed.
                shaderDecls.push_back(_decl);

                /// Stage-support gate. Metal still refuses hull/domain
                /// (OmegaSL-Reference.md bug 3); the diagnostic comes
                /// back through the out parameter.
                std::string supportDiag;
                if (!target->supportsStage(_decl->shaderType, supportDiag)) {
                    std::cerr << "error: " << supportDiag << " ('" << _decl->name << "')" << std::endl;
                    hasFatalErrors = true;
                    break;
                }

                /// Layer 1 stub path (Feature-Gap-Survey §14.1, user
                /// twist): one or more declared `#requires(...)` features
                /// have no `OMEGASL_FEATURE_<NAME>` macro on the active
                /// backend. Per the user request — "Emit the header with
                /// the capabilities to the lib but the code body will be
                /// empty." So we register an `omegasl_shader` entry with
                /// `type`, `name`, and `requiredFeatures` set, but write
                /// no source file and do not invoke the downstream
                /// compiler. The library writer recognizes the
                /// `stubShaderKeys` membership and emits a `dataSize == 0`
                /// record. The runtime loader then reports a precise
                /// "feature unavailable" diagnostic when a pipeline
                /// references the shader.
                if (fileUnsatisfiedFeatures != 0) {
                    omegasl_shader meta{};
                    meta.type = _decl->shaderType == ast::ShaderDecl::Vertex     ? OMEGASL_SHADER_VERTEX
                                : _decl->shaderType == ast::ShaderDecl::Fragment ? OMEGASL_SHADER_FRAGMENT
                                : _decl->shaderType == ast::ShaderDecl::Compute  ? OMEGASL_SHADER_COMPUTE
                                : _decl->shaderType == ast::ShaderDecl::Hull     ? OMEGASL_SHADER_HULL
                                                                                 : OMEGASL_SHADER_DOMAIN;
                    auto *nameBuf = new char[_decl->name.size() + 1];
                    std::copy(_decl->name.begin(), _decl->name.end(), nameBuf);
                    nameBuf[_decl->name.size()] = '\0';
                    meta.name = nameBuf;
                    meta.requiredFeatures = fileRequiredFeatures;
                    /// Synthetic key — not a real path; linkShaderObjects
                    /// uses `stubShaderKeys` membership to skip the file
                    /// read and write a header-only record.
                    OmegaCommon::String stubKey = std::string("__stub__:") + std::string(_decl->name);
                    stubShaderKeys.insert(std::string(stubKey));
                    shaderMap.insert(std::make_pair(stubKey, meta));
                    break;
                }

                OmegaCommon::String object_file;
                if (opts.runtimeCompile) {
                    object_file = _decl->name;
                    if (runtimeStringOut) runtimeStringOut->str("");
                } else {
                    object_file = OmegaCommon::FS::Path(opts.tempDir).append(_decl->name)
                                      .concat(target->shaderObjectFileExt(_decl->shaderType)).absPath();
                    fileOut.open(OmegaCommon::FS::Path(opts.tempDir).append(_decl->name)
                                     .concat(target->shaderFileExt(_decl->shaderType)).str(),
                                 std::ios::out);
                }

                /// Per-target preamble (MSL `#include <metal_stdlib>`,
                /// GLSL `#version 450`, HLSL nothing).
                target->emitDefaultHeaders(*this, shaderOut);

                /// Prototypes first (dedup by name), then bodies — supports
                /// forward declarations and calls between user functions
                /// regardless of definition order.
                {
                    OmegaCommon::Map<OmegaCommon::String, int> emittedProtos;
                    for (auto *uf : userFuncDecls) {
                        if (emittedProtos.find(uf->name) != emittedProtos.end()) continue;
                        emittedProtos.insert(std::make_pair(uf->name, 0));
                        emitUserFunctionPrototype(uf);
                    }
                }
                for (auto *uf : userFuncDecls) {
                    if (uf->isForwardDecl) continue;
                    emitUserFunction(uf);
                }

                /// HLSL/MSL emit cached struct text at file scope here;
                /// GLSL emits its own struct decls inside the entry header,
                /// so its hook is a no-op.
                target->emitShaderUsedStructs(*this, _decl, shaderOut);

                /// Shader entry: header (resources + signature), then
                /// body (block walk + target-specific pre/post amble).
                omegasl_shader meta{};
                target->emitShaderEntryHeader(*this, _decl, meta, shaderOut);
                target->emitShaderEntryBody(*this, _decl, meta, shaderOut);
                target->emitShaderEntryFooter(*this, _decl, shaderOut);

                if (!opts.runtimeCompile) {
                    fileOut.close();
                }
                /// Layer 1: tag every emitted shader with the file-scope
                /// `#requires(...)` bitfield so the runtime loader can
                /// reject only the shaders whose declared requirements
                /// aren't satisfied by the device. Other shaders in the
                /// same library load normally (Feature-Gap-Survey §14.3).
                meta.requiredFeatures = fileRequiredFeatures;
                shaderMap.insert(std::make_pair(object_file, meta));
                break;
            }
        }
    }

    /// Phase 10: factories. The caller picks the backend by constructing
    /// the matching Target subclass.
    std::shared_ptr<CodeGen> CodeGenMake(CodeGenOpts &opts, std::unique_ptr<Target> target) {
        return std::make_shared<CodeGen>(opts, std::move(target));
    }

    std::shared_ptr<CodeGen> CodeGenMakeRuntime(CodeGenOpts &opts, std::unique_ptr<Target> target,
                                                std::ostringstream &out) {
        return std::make_shared<CodeGen>(opts, std::move(target), out);
    }

    bool CodeGen::generateInterfaceAndCompileShader(ast::Decl *decl) {
        if(decl->type == SHADER_DECL){
            auto _decl = (ast::ShaderDecl *)decl;
            /// Layer 1 stub path: SHADER_DECL recorded a header-only
            /// entry because the active backend can't express one of
            /// the required features. There's no source file to compile.
            if(stubShaderKeys.count(std::string("__stub__:") + std::string(_decl->name)) > 0){
                return true;
            }
            /// If the backend already refused to emit this stage (e.g.
            /// Metal hull/domain), it has set hasFatalErrors and skipped
            /// writing the source file. Don't invoke the downstream
            /// compiler — it would just print a confusing
            /// "no such file" error on top of our diagnostic.
            if(hasFatalErrors){
                return true;
            }
            /// `--emit-source-only` mode: source has already been written
            /// to tempDir by generateDecl; skip the toolchain invocation
            /// entirely so this works on hosts that don't have dxc / metal
            /// / glslc available.
            if(opts.emitSourceOnly){
                return true;
            }
            if(opts.runtimeCompile){
                compileShaderOnRuntime(_decl->shaderType,_decl->name);
            }
            else {
                if(!compileShader(_decl->shaderType,_decl->name,opts.tempDir,opts.tempDir)){
                    return false;
                }
            }
        }
        return true;
    }
}
