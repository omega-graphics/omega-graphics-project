#ifndef OMEGASL_FEATURE_SCANNER_H
#define OMEGASL_FEATURE_SCANNER_H

#include "AST.h"

#include <cstdint>
#include <iosfwd>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace omegasl {

/// Layer 2 of the Backend Feature Gating system
/// (Feature-Gap-Survey §14.2). Walks every parsed `FuncDecl` /
/// `ShaderDecl` body looking for backend-asymmetric language constructs
/// — currently `read`/`write` calls on `texturecube[_array]` and
/// `write` calls on `texture2d_ms[_array]`. Records the matching
/// `OMEGASL_FEATURE_BIT_*` bits onto each decl's `usedFeatures` field,
/// then folds callee bits into callers via a fixed-point pass over the
/// call graph so a shader sees every feature its transitive user-
/// function callees use.
///
/// The scanner is *advisory*: nothing it produces affects codegen or
/// the emitted library. It exists to drive two warnings:
///   1. "feature X is used but file scope didn't `#requires(X)`" —
///      tells the author the runtime won't know to gate the shader.
///   2. "this file mixes shaders that use feature X with shaders that
///      don't — consider moving the using-shaders into
///      `<filename>_<feature>.omegasl` so the rest stays loadable on
///      devices without X."
class FeatureScanner {
public:
    FeatureScanner(ast::SemFrontend *sem,
                   std::vector<ast::FuncDecl *> userFuncs,
                   std::vector<ast::ShaderDecl *> shaderDecls);

    /// Scan every recorded `FuncDecl` / `ShaderDecl`, then propagate
    /// transitively through the call graph. After this returns each
    /// decl's `usedFeatures` reflects its own body uses unioned with
    /// every transitively called user function's uses.
    void run();

    /// Emit the two advisory diagnostics described in the class comment.
    /// `sourcePath` is the input filename (used in the partition
    /// suggestion's recommended new filename); `fileRequiredFeatures` is
    /// the union of `#requires(...)` directives the Preprocessor
    /// resolved at file scope. Writes to `out`.
    void emitDiagnostics(const std::string &sourcePath,
                         uint64_t fileRequiredFeatures,
                         std::ostream &out) const;

private:
    ast::SemFrontend *sem_;
    std::vector<ast::FuncDecl *> userFuncs_;
    std::vector<ast::ShaderDecl *> shaderDecls_;
    /// Lookup by name for resolving call-graph edges (callee name →
    /// callee FuncDecl). Forward declarations and full definitions
    /// share a name; we always pin to the entry whose body is
    /// non-empty when there's a choice.
    std::map<std::string, ast::FuncDecl *> userFuncByName_;
    /// `callGraph_[F]` = names of user functions called from F's body.
    /// Built during the per-decl scan; consumed by the fixed-point pass.
    std::map<ast::FuncDecl *, std::set<std::string>> callGraph_;

    void scanFuncBody(ast::FuncDecl *fn);
    void scanBlock(ast::Block *block, ast::FuncDecl *enclosing);
    void scanStmt(ast::Stmt *stmt, ast::FuncDecl *enclosing);
    void scanExpr(ast::Expr *expr, ast::FuncDecl *enclosing);

    /// Match a `read`/`write` `CallExpr` against the trigger table and
    /// OR the resulting feature bits into `enclosing->usedFeatures`.
    /// Records call edges (`callGraph_[enclosing]`) for user-function
    /// callees so the fixed-point pass can fold them in later.
    void inspectCall(ast::CallExpr *call, ast::FuncDecl *enclosing);

    /// §4.1 / §4.2 — inspect a declared type at any of the per-decl
    /// surfaces (function param type, function return type, var decl
    /// type, cast target type) and OR FLOAT16 / INT64 onto the enclosing
    /// FuncDecl when the type belongs to one of those families. Walks
    /// into user struct fields recursively so a struct literal that
    /// hides a `half` deep in its layout still trips the bit.
    void inspectTypeExpr(ast::TypeExpr *typeExpr, ast::FuncDecl *enclosing);
    /// Walk every parameter / return type / var decl type in a function
    /// — Pass-1 entry hook for type-driven feature scanning. Body
    /// expressions are still handled by `scanFuncBody`.
    void scanFuncTypes(ast::FuncDecl *fn);

    /// Memoization for struct-field walks. Keyed by `ast::Type *` (the
    /// resolved struct), value is the union of feature bits implied by
    /// the struct's fields. Avoids re-walking a struct used in multiple
    /// places and silently terminates on cycles (a struct that
    /// transitively references itself returns 0 the second time).
    std::map<ast::Type *, uint64_t> structTypeFeatureCache_;
    /// Sentinel value injected before computing a struct's feature
    /// signature so a recursive walk sees `in-progress` and returns 0
    /// for the back-edge instead of infinite-recursing.
    static constexpr uint64_t kStructInFlight = static_cast<uint64_t>(-1);
};

} // namespace omegasl

#endif
