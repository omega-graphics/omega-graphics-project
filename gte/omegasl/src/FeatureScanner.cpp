#include "FeatureScanner.h"

#include "AST.def"
#include "AST.h"

#include <omegasl.h>

#include <algorithm>
#include <ostream>
#include <sstream>

namespace omegasl {

namespace {

/// Lower-case spelling of a feature bit, used in the partition-
/// suggestion's recommended filename (`<file>_<lower>.omegasl`).
const char *featureLower(uint64_t bit) {
    switch (bit) {
        case OMEGASL_FEATURE_BIT_RAYTRACING:         return "raytracing";
        case OMEGASL_FEATURE_BIT_MESH_SHADERS:       return "mesh_shaders";
        case OMEGASL_FEATURE_BIT_GEOMETRY_SHADERS:   return "geometry_shaders";
        case OMEGASL_FEATURE_BIT_TESSELLATION:       return "tessellation";
        case OMEGASL_FEATURE_BIT_SUBGROUP_OPS:       return "subgroup_ops";
        case OMEGASL_FEATURE_BIT_BINDLESS:           return "bindless";
        case OMEGASL_FEATURE_BIT_FLOAT16:            return "float16";
        case OMEGASL_FEATURE_BIT_INT64:              return "int64";
        case OMEGASL_FEATURE_BIT_VARIABLE_RATE:      return "variable_rate";
        case OMEGASL_FEATURE_BIT_SUBPASS_INPUTS:     return "subpass_inputs";
        case OMEGASL_FEATURE_BIT_SPEC_CONSTANTS:     return "spec_constants";
        case OMEGASL_FEATURE_BIT_TEXTURECUBE_RW:     return "texturecube_rw";
        case OMEGASL_FEATURE_BIT_TEXTURE2D_MS_WRITE: return "texture2d_ms_write";
        case OMEGASL_FEATURE_BIT_DOUBLE:             return "double";
        default:                                     return "unknown";
    }
}

/// Upper-case macro suffix for the feature bit (matches the
/// Preprocessor's `OMEGASL_FEATURE_<NAME>` macro spelling).
const char *featureUpper(uint64_t bit) {
    switch (bit) {
        case OMEGASL_FEATURE_BIT_RAYTRACING:         return "RAYTRACING";
        case OMEGASL_FEATURE_BIT_MESH_SHADERS:       return "MESH_SHADERS";
        case OMEGASL_FEATURE_BIT_GEOMETRY_SHADERS:   return "GEOMETRY_SHADERS";
        case OMEGASL_FEATURE_BIT_TESSELLATION:       return "TESSELLATION";
        case OMEGASL_FEATURE_BIT_SUBGROUP_OPS:       return "SUBGROUP_OPS";
        case OMEGASL_FEATURE_BIT_BINDLESS:           return "BINDLESS";
        case OMEGASL_FEATURE_BIT_FLOAT16:            return "FLOAT16";
        case OMEGASL_FEATURE_BIT_INT64:              return "INT64";
        case OMEGASL_FEATURE_BIT_VARIABLE_RATE:      return "VARIABLE_RATE";
        case OMEGASL_FEATURE_BIT_SUBPASS_INPUTS:     return "SUBPASS_INPUTS";
        case OMEGASL_FEATURE_BIT_SPEC_CONSTANTS:     return "SPEC_CONSTANTS";
        case OMEGASL_FEATURE_BIT_TEXTURECUBE_RW:     return "TEXTURECUBE_RW";
        case OMEGASL_FEATURE_BIT_TEXTURE2D_MS_WRITE: return "TEXTURE2D_MS_WRITE";
        case OMEGASL_FEATURE_BIT_DOUBLE:             return "DOUBLE";
        default:                                     return "UNKNOWN";
    }
}

/// Resolve an argument expression's resolved type to its underlying
/// `ast::Type *` builtin (`texturecube_type`, `texture2d_ms_type`,
/// etc.). Returns nullptr when the expression has no resolved type yet
/// — happens when Sema bailed early on a previous error; in that case
/// we silently skip the trigger match.
ast::Type *resolveArgType(ast::SemFrontend *sem, ast::Expr *arg) {
    if (!arg || !arg->resolvedType) return nullptr;
    return sem->resolveTypeWithExpr(arg->resolvedType);
}

bool isCubeTexture(ast::Type *t) {
    return t == ast::builtins::texturecube_type ||
           t == ast::builtins::texturecube_array_type;
}

bool isMSTexture(ast::Type *t) {
    return t == ast::builtins::texture2d_ms_type ||
           t == ast::builtins::texture2d_ms_array_type;
}

/// §4.1 / §4.2 — feature bits implied by a *resolved* builtin Type.
/// Returns 0 for types that don't trip a portability gate (`int`,
/// `float`, textures, etc.). User struct types are handled by
/// `featureBitsForStructFields` so the recursion can detect cycles.
uint64_t featureBitsForBuiltin(ast::Type *t) {
    using namespace ast::builtins;
    if (t == nullptr) return 0;
    /// 16-bit family — float16 + int16/uint16 share OMEGASL_FEATURE_BIT_FLOAT16
    /// per the survey rationale (every modern surface ships them
    /// together).
    if (t == half_type   || t == half2_type   || t == half3_type   || t == half4_type   ||
        t == short_type  || t == short2_type  || t == short3_type  || t == short4_type  ||
        t == ushort_type || t == ushort2_type || t == ushort3_type || t == ushort4_type) {
        return OMEGASL_FEATURE_BIT_FLOAT16;
    }
    /// 64-bit ints
    if (t == long_type   || t == long2_type   || t == long3_type   || t == long4_type   ||
        t == ulong_type  || t == ulong2_type  || t == ulong3_type  || t == ulong4_type) {
        return OMEGASL_FEATURE_BIT_INT64;
    }
    return 0;
}

} // namespace

FeatureScanner::FeatureScanner(ast::SemFrontend *sem,
                               std::vector<ast::FuncDecl *> userFuncs,
                               std::vector<ast::ShaderDecl *> shaderDecls)
    : sem_(sem),
      userFuncs_(std::move(userFuncs)),
      shaderDecls_(std::move(shaderDecls)) {
    /// Build the by-name lookup. When the same name appears more than
    /// once (forward decl + definition), prefer the entry that has a
    /// body so the fixed-point pass walks the actual implementation.
    for (auto *fn : userFuncs_) {
        auto it = userFuncByName_.find(fn->name);
        if (it == userFuncByName_.end()) {
            userFuncByName_.emplace(fn->name, fn);
        } else if (fn->block && !it->second->block) {
            it->second = fn;
        }
    }
}

void FeatureScanner::run() {
    /// Pass 1a: type-driven feature use. Walks the static surface of
    /// each function — params, return type, var decl types, cast
    /// targets — and trips FLOAT16 / INT64 when those types appear.
    /// Done *before* the body walk so the bits are available when
    /// fold-in propagates through the call graph.
    for (auto *fn : userFuncs_) {
        scanFuncTypes(fn);
    }
    for (auto *sd : shaderDecls_) {
        scanFuncTypes(sd);
    }

    /// Pass 1b: per-decl direct uses (existing call-trigger path).
    /// After this `decl->usedFeatures` reflects what the body itself
    /// touches plus the type-driven bits from Pass 1a.
    for (auto *fn : userFuncs_) {
        scanFuncBody(fn);
    }
    for (auto *sd : shaderDecls_) {
        scanFuncBody(sd);
    }

    /// Pass 2: fixed-point closure over the call graph. A caller picks
    /// up each callee's `usedFeatures` and we keep iterating until no
    /// more bits flow. Cycles are bounded — at most one iteration adds
    /// each bit so the loop terminates after at worst (decls × bits)
    /// iterations.
    bool changed = true;
    while (changed) {
        changed = false;
        auto fold = [&](ast::FuncDecl *caller) {
            auto edgesIt = callGraph_.find(caller);
            if (edgesIt == callGraph_.end()) return;
            for (const auto &calleeName : edgesIt->second) {
                auto calleeIt = userFuncByName_.find(calleeName);
                if (calleeIt == userFuncByName_.end()) continue;
                uint64_t before = caller->usedFeatures;
                caller->usedFeatures |= calleeIt->second->usedFeatures;
                if (caller->usedFeatures != before) changed = true;
            }
        };
        for (auto *fn : userFuncs_) fold(fn);
        for (auto *sd : shaderDecls_) fold(sd);
    }
}

void FeatureScanner::scanFuncBody(ast::FuncDecl *fn) {
    if (!fn || !fn->block) return;
    scanBlock(fn->block.get(), fn);
}

void FeatureScanner::scanBlock(ast::Block *block, ast::FuncDecl *enclosing) {
    if (!block) return;
    for (auto *stmt : block->body) {
        scanStmt(stmt, enclosing);
    }
}

void FeatureScanner::scanStmt(ast::Stmt *stmt, ast::FuncDecl *enclosing) {
    if (!stmt) return;
    switch (stmt->type) {
        case VAR_DECL: {
            auto *vd = static_cast<ast::VarDecl *>(stmt);
            /// Local-variable type counts as a use of any feature-gated
            /// type it names — `half x; long y;` should trip the bits.
            inspectTypeExpr(vd->typeExpr, enclosing);
            if (vd->spec.initializer.has_value()) {
                scanExpr(vd->spec.initializer.value(), enclosing);
            }
            break;
        }
        case RETURN_DECL: {
            auto *rd = static_cast<ast::ReturnDecl *>(stmt);
            if (rd->expr) scanExpr(rd->expr, enclosing);
            break;
        }
        case IF_STMT: {
            auto *is = static_cast<ast::IfStmt *>(stmt);
            scanExpr(is->condition, enclosing);
            scanBlock(is->thenBlock.get(), enclosing);
            for (auto &branch : is->elseIfs) {
                scanExpr(branch.condition, enclosing);
                scanBlock(branch.block.get(), enclosing);
            }
            if (is->elseBlock) scanBlock(is->elseBlock.get(), enclosing);
            break;
        }
        case FOR_STMT: {
            auto *fs = static_cast<ast::ForStmt *>(stmt);
            if (fs->init) scanStmt(fs->init, enclosing);
            if (fs->condition) scanExpr(fs->condition, enclosing);
            if (fs->increment) scanExpr(fs->increment, enclosing);
            scanBlock(fs->body.get(), enclosing);
            break;
        }
        case WHILE_STMT: {
            auto *ws = static_cast<ast::WhileStmt *>(stmt);
            if (ws->condition) scanExpr(ws->condition, enclosing);
            scanBlock(ws->body.get(), enclosing);
            break;
        }
        case SWITCH_STMT: {
            auto *ss = static_cast<ast::SwitchStmt *>(stmt);
            scanExpr(ss->condition, enclosing);
            for (auto &c : ss->cases) {
                if (c.value) scanExpr(c.value, enclosing);
                for (auto *s : c.body) scanStmt(s, enclosing);
            }
            break;
        }
        case BREAK_STMT:
        case CONTINUE_STMT:
        case DISCARD_STMT:
            break;
        default:
            /// Anything else is treated as an expression statement —
            /// the parser stores expression statements as raw `Expr*`
            /// inside the block body.
            if ((stmt->type & EXPR) == EXPR) {
                scanExpr(static_cast<ast::Expr *>(stmt), enclosing);
            }
            break;
    }
}

void FeatureScanner::scanExpr(ast::Expr *expr, ast::FuncDecl *enclosing) {
    if (!expr) return;
    switch (expr->type) {
        case CALL_EXPR: {
            auto *call = static_cast<ast::CallExpr *>(expr);
            inspectCall(call, enclosing);
            /// Recurse into args — a feature-tripping construct can be
            /// nested inside another call (e.g. `pack(write(cubeT, ...))`).
            scanExpr(call->callee, enclosing);
            for (auto *a : call->args) scanExpr(a, enclosing);
            break;
        }
        case MEMBER_EXPR: {
            auto *m = static_cast<ast::MemberExpr *>(expr);
            scanExpr(m->lhs, enclosing);
            break;
        }
        case BINARY_EXPR: {
            auto *b = static_cast<ast::BinaryExpr *>(expr);
            scanExpr(b->lhs, enclosing);
            scanExpr(b->rhs, enclosing);
            break;
        }
        case UNARY_EXPR: {
            auto *u = static_cast<ast::UnaryOpExpr *>(expr);
            scanExpr(u->expr, enclosing);
            break;
        }
        case POINTER_EXPR: {
            auto *p = static_cast<ast::PointerExpr *>(expr);
            scanExpr(p->expr, enclosing);
            break;
        }
        case INDEX_EXPR: {
            auto *ix = static_cast<ast::IndexExpr *>(expr);
            scanExpr(ix->lhs, enclosing);
            scanExpr(ix->idx_expr, enclosing);
            break;
        }
        case CAST_EXPR: {
            auto *c = static_cast<ast::CastExpr *>(expr);
            /// `(half)x` is a use of `half` even when the source isn't.
            inspectTypeExpr(c->targetType, enclosing);
            scanExpr(c->expr, enclosing);
            break;
        }
        case TERNARY_EXPR: {
            /// §3.2 — walk all three children so a feature-tripping
            /// construct nested in any branch (or the condition) gets
            /// counted.
            auto *t = static_cast<ast::TernaryExpr *>(expr);
            scanExpr(t->condition, enclosing);
            scanExpr(t->thenExpr, enclosing);
            scanExpr(t->elseExpr, enclosing);
            break;
        }
        case ARRAY_EXPR: {
            auto *a = static_cast<ast::ArrayExpr *>(expr);
            for (auto *e : a->elm) scanExpr(e, enclosing);
            break;
        }
        case ID_EXPR:
        case LITERAL_EXPR:
        default:
            break;
    }
}

void FeatureScanner::inspectTypeExpr(ast::TypeExpr *typeExpr, ast::FuncDecl *enclosing) {
    if (!typeExpr || !enclosing) return;

    ast::Type *resolved = sem_->resolveTypeWithExpr(typeExpr);
    if (!resolved) return;

    /// Direct hit: the TypeExpr names a builtin in the new families.
    enclosing->usedFeatures |= featureBitsForBuiltin(resolved);

    /// `buffer<T>` and similar generics carry their element type as
    /// `typeExpr->args[0]`. Walk the args so a `buffer<half>` resource
    /// counts as a FLOAT16 use even though the outer type is the
    /// neutral `buffer_type`.
    if (typeExpr->hasTypeArgs) {
        for (auto *arg : typeExpr->args) {
            inspectTypeExpr(arg, enclosing);
        }
    }

    /// User struct: walk fields and union the bits. Memoize the result
    /// per resolved struct since the same type often appears in many
    /// places. The kStructInFlight sentinel breaks self-referential
    /// recursion (returns 0 for the back-edge).
    if (!resolved->builtin && !resolved->fields.empty()) {
        auto cacheIt = structTypeFeatureCache_.find(resolved);
        if (cacheIt != structTypeFeatureCache_.end()) {
            uint64_t cached = cacheIt->second;
            if (cached != kStructInFlight) {
                enclosing->usedFeatures |= cached;
            }
            return;
        }
        structTypeFeatureCache_[resolved] = kStructInFlight;
        uint64_t bits = 0;
        for (auto &kv : resolved->fields) {
            ast::TypeExpr *fieldTy = kv.second;
            if (!fieldTy) continue;
            ast::Type *fieldResolved = sem_->resolveTypeWithExpr(fieldTy);
            if (!fieldResolved) continue;
            bits |= featureBitsForBuiltin(fieldResolved);
            /// Recurse into nested user structs and generic args.
            if (fieldTy->hasTypeArgs) {
                for (auto *arg : fieldTy->args) {
                    inspectTypeExpr(arg, enclosing);
                }
            }
            if (!fieldResolved->builtin) {
                inspectTypeExpr(fieldTy, enclosing);
            }
        }
        structTypeFeatureCache_[resolved] = bits;
        enclosing->usedFeatures |= bits;
    }
}

void FeatureScanner::scanFuncTypes(ast::FuncDecl *fn) {
    if (!fn) return;
    /// Function/shader signature: parameters and return type.
    for (auto &p : fn->params) {
        inspectTypeExpr(p.typeExpr, fn);
    }
    inspectTypeExpr(fn->returnType, fn);
    /// Note: var decl + cast types are handled in the body walk so we
    /// don't double-count them here. Same for nested struct fields,
    /// which are visited when a struct-typed VarDecl shows up.
}

void FeatureScanner::inspectCall(ast::CallExpr *call, ast::FuncDecl *enclosing) {
    if (!call->callee || call->callee->type != ID_EXPR) return;
    auto *id = static_cast<ast::IdExpr *>(call->callee);
    const auto &name = id->id;

    /// Trigger table — backend-asymmetric texture ops on cube/MS textures.
    if (name == BUILTIN_READ || name == BUILTIN_WRITE) {
        if (!call->args.empty()) {
            ast::Type *t = resolveArgType(sem_, call->args[0]);
            if (isCubeTexture(t)) {
                enclosing->usedFeatures |= OMEGASL_FEATURE_BIT_TEXTURECUBE_RW;
            } else if (name == BUILTIN_WRITE && isMSTexture(t)) {
                enclosing->usedFeatures |= OMEGASL_FEATURE_BIT_TEXTURE2D_MS_WRITE;
            }
        }
    }

    /// Record a call-graph edge if the callee is a user function. The
    /// fixed-point pass folds the callee's `usedFeatures` into ours.
    auto userIt = userFuncByName_.find(std::string(name));
    if (userIt != userFuncByName_.end()) {
        callGraph_[enclosing].insert(std::string(name));
    }
}

void FeatureScanner::emitDiagnostics(const std::string &sourcePath,
                                     uint64_t fileRequiredFeatures,
                                     std::ostream &out) const {
    if (shaderDecls_.empty()) return;

    /// Collect, per feature bit, the names of shaders that use it.
    std::map<uint64_t, std::vector<std::string>> shadersUsing;
    for (auto *sd : shaderDecls_) {
        uint64_t bits = sd->usedFeatures;
        while (bits) {
            uint64_t low = bits & -bits;
            shadersUsing[low].push_back(sd->name);
            bits &= bits - 1;
        }
    }

    if (shadersUsing.empty()) return;

    /// Strip any directory components for the partition suggestion's
    /// recommended filename so the printed path doesn't drag in
    /// unrelated parents.
    std::string baseName = sourcePath;
    auto slash = baseName.find_last_of("/\\");
    if (slash != std::string::npos) baseName.erase(0, slash + 1);
    /// Drop the .omegasl extension if present so the synthesized name
    /// doesn't double-suffix.
    std::string stem = baseName;
    auto dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem.erase(dot);

    for (const auto &p : shadersUsing) {
        uint64_t bit = p.first;
        const auto &usingNames = p.second;
        const char *upper = featureUpper(bit);
        const char *lower = featureLower(bit);

        /// (1) Undeclared-use warning: at least one shader uses this
        /// feature but the file scope didn't declare `#requires(X)`.
        /// Without the directive, the lib record won't tag the bit and
        /// the runtime can't reject incompatible devices precisely.
        if ((fileRequiredFeatures & bit) == 0) {
            out << "warning: ";
            if (usingNames.size() == 1) {
                out << "shader '" << usingNames[0] << "'";
            } else {
                out << "shaders ";
                for (size_t i = 0; i < usingNames.size(); ++i) {
                    out << (i == 0 ? "'" : ", '") << usingNames[i] << "'";
                }
            }
            out << " in " << baseName << " use feature " << upper
                << " but no `#requires(" << upper
                << ")` declaration was found at file scope. Add one at "
                   "the top of the file so the runtime gates the shader "
                   "on incompatible devices."
                << std::endl;
        }

        /// (2) Partition suggestion: a feature is used by a strict
        /// subset of shaders. Suggest splitting so the unaffected
        /// shaders stay loadable on devices without the feature.
        if (usingNames.size() < shaderDecls_.size()) {
            out << "warning: " << baseName << " mixes shaders that use "
                << upper << " (";
            for (size_t i = 0; i < usingNames.size(); ++i) {
                out << (i == 0 ? "'" : ", '") << usingNames[i] << "'";
            }
            out << ") with " << (shaderDecls_.size() - usingNames.size())
                << " shader(s) that don't. Consider moving the gated shaders "
                   "into "
                << stem << "_" << lower
                << ".omegasl so the rest stay loadable on devices without "
                << upper << "." << std::endl;
        }
    }
}

} // namespace omegasl
