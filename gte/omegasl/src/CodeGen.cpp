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
                generateExpr(_expr->lhs);
                out << "[";
                generateExpr(_expr->idx_expr);
                out << "]";
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
                } else if (_id_expr == BUILTIN_WRITE) {
                    generatedExprBody = true;
                    target->emitTextureWrite(*this, _expr, out);
                } else if (_id_expr == BUILTIN_READ) {
                    generatedExprBody = true;
                    target->emitTextureRead(*this, _expr, out);
                } else if (isUserFunc(_id_expr)) {
                    out << spellUserFuncName(_id_expr);
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
                || stmt->type == CONTINUE_STMT || stmt->type == DISCARD_STMT) {
                generateDecl((ast::Decl *)stmt);
                if (stmt->type != IF_STMT && stmt->type != FOR_STMT && stmt->type != WHILE_STMT) {
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
            layout.push_back(layoutDesc);
        }
        target->emitStaticPreamble(out);
        meta.nLayout = layout.size();
        if (!layout.empty()) {
            meta.pLayout = new omegasl_shader_layout_desc[layout.size()];
            std::copy(layout.begin(), layout.end(), meta.pLayout);
        }
    }

    bool CodeGen::generateInterfaceAndCompileShader(ast::Decl *decl) {
        if(decl->type == SHADER_DECL){
            auto _decl = (ast::ShaderDecl *)decl;
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
