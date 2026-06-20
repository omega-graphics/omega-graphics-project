#include "Analysis.h"

#include "../src/Preprocessor.h"
#include "../src/Parser.h"
#include "../src/CodeGen.h"
#include "../src/Error.h"
#include "../src/AST.h"

#include <sstream>

namespace omegasl {
namespace lsp {

    namespace {

        /// Host-default backend, matching `omegaslc`'s `defaultGenModeForHost`.
        /// The server runs frontend-only, so the choice only affects which
        /// `OMEGASL_BACKEND_*` / `OMEGASL_FEATURE_*` macros the preprocessor
        /// predefines (so `#if defined(...)` / `#requires(...)` resolve the way
        /// they would when compiling on this host).
        PPBackend hostPPBackend() {
        #if defined(_WIN32)
            return PPBackend::HLSL;
        #elif defined(__APPLE__)
            return PPBackend::MSL;
        #else
            return PPBackend::GLSL;
        #endif
        }

        /// Construct the host-default `CodeGen`. Its emission hooks are never
        /// invoked (the parser runs in frontend-only mode), but the `Parser`
        /// constructor requires a `CodeGen` to wire the type resolver, so we
        /// build a real one. No GPU device is touched.
        std::shared_ptr<CodeGen> makeHostCodeGen(CodeGenOpts & opts) {
        #if defined(_WIN32)
            HLSLCodeOpts targetOpts {};
            return CodeGenMake(opts, std::make_unique<HLSLTarget>(targetOpts));
        #elif defined(__APPLE__)
            MetalCodeOpts targetOpts {};
            return CodeGenMake(opts, std::make_unique<MSLTarget>(targetOpts));
        #else
            GLSLCodeOpts targetOpts {};
            return CodeGenMake(opts, std::make_unique<GLSLTarget>(targetOpts));
        #endif
        }

        /// LSP positions are 0-based line / character; OmegaSL `ErrorLoc` is
        /// 1-based line / 0-based column. Convert, and widen a zero-width span
        /// to one character so the editor has something to underline.
        Range rangeFromErrorLoc(const ErrorLoc & loc) {
            Range r;
            r.startLine = loc.lineStart > 0 ? loc.lineStart - 1 : 0;
            r.endLine = loc.lineEnd > 0 ? loc.lineEnd - 1 : r.startLine;
            r.startChar = loc.colStart;
            r.endChar = loc.colEnd;
            if (r.endLine == r.startLine && r.endChar <= r.startChar) {
                r.endChar = r.startChar + 1;
            }
            return r;
        }

        /// Spell a `TypeExpr` back into OmegaSL surface syntax:
        /// `buffer<MyVertex>`, `float4`, `T *`, `float[16][16]`.
        std::string renderTypeExpr(ast::TypeExpr * t) {
            if (t == nullptr) {
                return "?";
            }
            std::string s(t->name.data(), t->name.size());
            if (t->hasTypeArgs && !t->args.empty()) {
                s += "<";
                bool first = true;
                for (auto * arg : t->args) {
                    if (!first) {
                        s += ", ";
                    }
                    first = false;
                    s += renderTypeExpr(arg);
                }
                s += ">";
            }
            if (t->pointer) {
                s += " *";
            }
            for (auto dim : t->arrayDims) {
                s += "[" + std::to_string(dim) + "]";
            }
            return s;
        }

        std::string renderParamList(ast::FuncDecl * fn) {
            std::string s = "(";
            bool first = true;
            for (auto & p : fn->params) {
                if (!first) {
                    s += ", ";
                }
                first = false;
                if (p.isConst) {
                    s += "const ";
                }
                s += renderTypeExpr(p.typeExpr);
                if (!p.name.empty()) {
                    s += " ";
                    s += std::string(p.name.data(), p.name.size());
                }
            }
            s += ")";
            return s;
        }

        const char * stageKeyword(ast::ShaderDecl::Type ty) {
            switch (ty) {
                case ast::ShaderDecl::Vertex:   return "vertex";
                case ast::ShaderDecl::Fragment: return "fragment";
                case ast::ShaderDecl::Compute:  return "compute";
                case ast::ShaderDecl::Hull:     return "hull";
                case ast::ShaderDecl::Domain:   return "domain";
                case ast::ShaderDecl::Mesh:     return "mesh";
            }
            return "shader";
        }

        SymbolKind shaderSymbolKind(ast::ShaderDecl::Type ty) {
            switch (ty) {
                case ast::ShaderDecl::Vertex:   return SymbolKind::VertexShader;
                case ast::ShaderDecl::Fragment: return SymbolKind::FragmentShader;
                case ast::ShaderDecl::Compute:  return SymbolKind::ComputeShader;
                case ast::ShaderDecl::Hull:     return SymbolKind::HullShader;
                case ast::ShaderDecl::Domain:   return SymbolKind::DomainShader;
                case ast::ShaderDecl::Mesh:     return SymbolKind::MeshShader;
            }
            return SymbolKind::Function;
        }

        /// Classify a resource by the head of its type spelling so hover and
        /// the outline can show a texture/sampler/buffer glyph.
        SymbolKind resourceSymbolKind(const std::string & typeName) {
            if (typeName.rfind("texture", 0) == 0) {
                return SymbolKind::Texture;
            }
            if (typeName.rfind("sampler", 0) == 0) {
                return SymbolKind::Sampler;
            }
            return SymbolKind::Buffer;
        }

        /// Build a `Symbol` for one top-level declaration, or report failure
        /// (false) for a node kind the outline doesn't surface.
        bool buildSymbol(ast::Decl * decl, Symbol & out) {
            using namespace ast;
            if (!decl->loc) {
                return false;
            }
            out.range = rangeFromErrorLoc(*decl->loc);

            switch (decl->type) {
                case STRUCT_DECL: {
                    auto * s = (StructDecl *)decl;
                    out.name = std::string(s->name.data(), s->name.size());
                    out.kind = SymbolKind::Struct;
                    out.detail = s->internal ? "internal struct" : "struct";
                    std::string sig = "struct " + out.name;
                    if (s->internal) {
                        sig += " internal";
                    }
                    sig += " {";
                    for (auto & f : s->fields) {
                        sig += "\n    " + renderTypeExpr(f.typeExpr) + " " +
                               std::string(f.name.data(), f.name.size());
                        if (f.attributeName) {
                            sig += " : " + std::string(f.attributeName->data(), f.attributeName->size());
                        }
                        sig += ";";
                    }
                    sig += "\n}";
                    out.signature = sig;
                    return true;
                }
                case SHADER_DECL: {
                    auto * sh = (ShaderDecl *)decl;
                    out.name = std::string(sh->name.data(), sh->name.size());
                    out.kind = shaderSymbolKind(sh->shaderType);
                    out.detail = std::string(stageKeyword(sh->shaderType)) + " shader";
                    out.signature = std::string(stageKeyword(sh->shaderType)) + " " +
                                    renderTypeExpr(sh->returnType) + " " + out.name +
                                    renderParamList(sh);
                    return true;
                }
                case FUNC_DECL: {
                    auto * fn = (FuncDecl *)decl;
                    out.name = std::string(fn->name.data(), fn->name.size());
                    out.kind = SymbolKind::Function;
                    out.detail = renderTypeExpr(fn->returnType);
                    out.signature = renderTypeExpr(fn->returnType) + " " + out.name + renderParamList(fn);
                    if (fn->isForwardDecl) {
                        out.signature += ";";
                        out.detail += " (forward)";
                    }
                    return true;
                }
                case RESOURCE_DECL: {
                    auto * r = (ResourceDecl *)decl;
                    out.name = std::string(r->name.data(), r->name.size());
                    std::string typeName = renderTypeExpr(r->typeExpr);
                    out.kind = resourceSymbolKind(typeName);
                    out.detail = typeName;
                    std::string sig;
                    if (r->isStatic) {
                        sig += "static ";
                    }
                    sig += typeName + " " + out.name + " : " + std::to_string(r->registerNumber);
                    out.signature = sig;
                    return true;
                }
                case VAR_DECL: {
                    auto * v = (VarDecl *)decl;
                    out.name = std::string(v->spec.name.data(), v->spec.name.size());
                    out.kind = SymbolKind::Constant;
                    out.detail = renderTypeExpr(v->typeExpr);
                    out.signature = (v->isConst ? std::string("const ") : std::string()) +
                                    renderTypeExpr(v->typeExpr) + " " + out.name;
                    return true;
                }
                default:
                    return false;
            }
        }

    } // namespace

    Analyzer::Analyzer() {
        ast::builtins::Initialize();
    }

    Analyzer::~Analyzer() {
        ast::builtins::Cleanup();
    }

    AnalysisResult Analyzer::analyze(const std::string & text) {
        AnalysisResult result;

        /// 1. Preprocess. A fresh instance per call keeps the sticky
        ///    `hasErrors`/`requiredFeatures` state isolated between edits.
        Preprocessor preprocessor;
        preprocessor.setBackend(hostPPBackend());
        /// No file-system anchor for an in-editor buffer: includes can't be
        /// resolved relative to a path we don't have, so reject them loudly
        /// rather than silently dropping declarations (mirrors the runtime
        /// `OmegaSLCompiler` path).
        preprocessor.setRejectIncludes(true);
        /// Keep output line numbers aligned 1:1 with the editor's buffer, so
        /// diagnostics and symbol locations land on the right line even when
        /// `#define` / `#ifdef` directives or skipped regions sit above them.
        preprocessor.setLinePreserving(true);
        std::string processed = preprocessor.process(text);

        if (preprocessor.hasErrors()) {
            /// The preprocessor reports include rejections to stderr without a
            /// structured location. Surface a file-scope diagnostic so the
            /// editor still flags that analysis was degraded.
            Diagnostic d;
            d.range = Range{0, 0, 0, 1};
            d.message = "preprocessor error (e.g. unsupported `#include` in an editor buffer); "
                        "see server log";
            result.diagnostics.push_back(d);
        }

        SourceFile sourceFile;
        sourceFile.setContent(processed);
        sourceFile.buildLinePosMap();

        DiagnosticEngine diagnostics;
        diagnostics.setSourceFile(&sourceFile);

        std::istringstream in(processed);

        /// 2. Parse + semantic analysis, frontend-only (no codegen / toolchain).
        CodeGenOpts codeGenOpts {
            /*emitHeaderOnly*/ false,
            /*runtimeCompile*/ false,
            /*emitSourceOnly*/ true,
            /*outputLib*/ OmegaCommon::StrRef(""),
            /*tempDir*/ OmegaCommon::StrRef("")
        };
        std::shared_ptr<CodeGen> codeGen = makeHostCodeGen(codeGenOpts);

        std::vector<ast::Decl *> decls;
        Parser parser(codeGen);
        ParseContext parseCtx{ in };
        parseCtx.sourceFile = &sourceFile;
        parseCtx.diagnostics = &diagnostics;
        parseCtx.frontendOnly = true;
        parseCtx.collectedDecls = &decls;
        parser.parseContext(parseCtx);

        /// 3a. Diagnostics straight from the compiler's engine.
        ///
        /// The front-end is a batch compiler: when a global declaration fails
        /// semantic analysis, `parseContext` appends a generic, *unlocated*
        /// "Failed to evaluate statement" marker and stops. That marker always
        /// trails a real, located error, and with no location it would
        /// otherwise underline line 0 — misleading in an editor. So drop
        /// unlocated diagnostics whenever a located one is present; a lone
        /// unlocated error (the only thing wrong) is still surfaced.
        bool anyLocated = false;
        for (const auto & err : diagnostics.getErrors()) {
            if (err->loc.lineStart > 0) {
                anyLocated = true;
                break;
            }
        }
        for (const auto & err : diagnostics.getErrors()) {
            if (err->loc.lineStart == 0 && anyLocated) {
                continue;
            }
            Diagnostic d;
            d.range = rangeFromErrorLoc(err->loc);
            std::ostringstream msg;
            err->format(msg);
            d.message = msg.str();
            d.severity = Severity::Error;
            result.diagnostics.push_back(d);
        }

        /// 3b. Symbols from the collected top-level declarations.
        for (auto * decl : decls) {
            Symbol sym;
            if (buildSymbol(decl, sym)) {
                result.index[sym.name] = sym;
                result.symbols.push_back(std::move(sym));
            }
        }

        return result;
    }

} // namespace lsp
} // namespace omegasl
