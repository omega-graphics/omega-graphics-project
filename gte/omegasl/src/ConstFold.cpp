#include "ConstFold.h"
#include "Toks.def"

namespace omegasl {

    /// Try to fold an expression. Returns a new LiteralExpr if the expression
    /// was fully constant, or the original (possibly mutated) expression otherwise.
    static ast::Expr *foldExpr(ast::Expr *expr);

    /// Fold all expressions reachable from a block.
    static void foldBlock(ast::Block &block);

    // ------------------------------------------------------------------
    // Helpers to build literal nodes
    // ------------------------------------------------------------------

    static ast::LiteralExpr *makeIntLit(int v) {
        auto *lit = new ast::LiteralExpr();
        lit->type = LITERAL_EXPR;
        lit->i_num = v;
        return lit;
    }

    static ast::LiteralExpr *makeUintLit(unsigned v) {
        auto *lit = new ast::LiteralExpr();
        lit->type = LITERAL_EXPR;
        lit->ui_num = v;
        return lit;
    }

    static ast::LiteralExpr *makeFloatLit(float v) {
        auto *lit = new ast::LiteralExpr();
        lit->type = LITERAL_EXPR;
        lit->f_num = v;
        return lit;
    }

    static ast::LiteralExpr *makeDoubleLit(double v) {
        auto *lit = new ast::LiteralExpr();
        lit->type = LITERAL_EXPR;
        lit->d_num = v;
        return lit;
    }

    // ------------------------------------------------------------------
    // Binary folding on two literals of the same numeric type
    // ------------------------------------------------------------------

    static ast::Expr *tryFoldBinary(ast::BinaryExpr *expr) {
        if (expr->lhs->type != LITERAL_EXPR || expr->rhs->type != LITERAL_EXPR) return expr;
        auto *lhs = static_cast<ast::LiteralExpr *>(expr->lhs);
        auto *rhs = static_cast<ast::LiteralExpr *>(expr->rhs);

        const auto &op = expr->op;

        // float op float
        if (lhs->isFloat() && rhs->isFloat()) {
            float a = lhs->f_num.value(), b = rhs->f_num.value();
            if (op == OP_PLUS)  return makeFloatLit(a + b);
            if (op == OP_MINUS) return makeFloatLit(a - b);
            if (op == "*")      return makeFloatLit(a * b);
            if (op == OP_DIV && b != 0.0f) return makeFloatLit(a / b);
        }

        // int op int
        if (lhs->isInt() && rhs->isInt()) {
            int a = lhs->i_num.value(), b = rhs->i_num.value();
            if (op == OP_PLUS)  return makeIntLit(a + b);
            if (op == OP_MINUS) return makeIntLit(a - b);
            if (op == "*")      return makeIntLit(a * b);
            if (op == OP_DIV && b != 0) return makeIntLit(a / b);
        }

        // uint op uint
        if (lhs->isUint() && rhs->isUint()) {
            unsigned a = lhs->ui_num.value(), b = rhs->ui_num.value();
            if (op == OP_PLUS)  return makeUintLit(a + b);
            if (op == OP_MINUS) return makeUintLit(a - b);
            if (op == "*")      return makeUintLit(a * b);
            if (op == OP_DIV && b != 0) return makeUintLit(a / b);
        }

        // double op double
        if (lhs->isDouble() && rhs->isDouble()) {
            double a = lhs->d_num.value(), b = rhs->d_num.value();
            if (op == OP_PLUS)  return makeDoubleLit(a + b);
            if (op == OP_MINUS) return makeDoubleLit(a - b);
            if (op == "*")      return makeDoubleLit(a * b);
            if (op == OP_DIV && b != 0.0) return makeDoubleLit(a / b);
        }

        return expr;
    }

    // ------------------------------------------------------------------
    // Unary folding on a literal operand
    // ------------------------------------------------------------------

    static ast::Expr *tryFoldUnary(ast::UnaryOpExpr *expr) {
        if (expr->expr->type != LITERAL_EXPR || !expr->isPrefix) return expr;
        auto *operand = static_cast<ast::LiteralExpr *>(expr->expr);

        const auto &op = expr->op;

        if (op == OP_MINUS) {
            if (operand->isFloat())  return makeFloatLit(-operand->f_num.value());
            if (operand->isInt())    return makeIntLit(-operand->i_num.value());
            if (operand->isDouble()) return makeDoubleLit(-operand->d_num.value());
        }

        return expr;
    }

    // ------------------------------------------------------------------
    // Recursive expression fold
    // ------------------------------------------------------------------

    static ast::Expr *foldExpr(ast::Expr *expr) {
        if (!expr) return expr;

        switch (expr->type) {
            case BINARY_EXPR: {
                auto *e = static_cast<ast::BinaryExpr *>(expr);
                e->lhs = foldExpr(e->lhs);
                e->rhs = foldExpr(e->rhs);
                return tryFoldBinary(e);
            }
            case UNARY_EXPR: {
                auto *e = static_cast<ast::UnaryOpExpr *>(expr);
                e->expr = foldExpr(e->expr);
                return tryFoldUnary(e);
            }
            case CALL_EXPR: {
                auto *e = static_cast<ast::CallExpr *>(expr);
                for (auto &arg : e->args) {
                    arg = foldExpr(arg);
                }
                return expr;
            }
            case MEMBER_EXPR: {
                auto *e = static_cast<ast::MemberExpr *>(expr);
                e->lhs = foldExpr(e->lhs);
                return expr;
            }
            case INDEX_EXPR: {
                auto *e = static_cast<ast::IndexExpr *>(expr);
                e->lhs = foldExpr(e->lhs);
                e->idx_expr = foldExpr(e->idx_expr);
                return expr;
            }
            case CAST_EXPR: {
                auto *e = static_cast<ast::CastExpr *>(expr);
                e->expr = foldExpr(e->expr);
                return expr;
            }
            case POINTER_EXPR: {
                auto *e = static_cast<ast::PointerExpr *>(expr);
                e->expr = foldExpr(e->expr);
                return expr;
            }
            case ARRAY_EXPR: {
                auto *e = static_cast<ast::ArrayExpr *>(expr);
                for (auto &el : e->elm) {
                    el = foldExpr(el);
                }
                return expr;
            }
            default:
                return expr;
        }
    }

    // ------------------------------------------------------------------
    // Statement / declaration walkers
    // ------------------------------------------------------------------

    static void foldStmt(ast::Stmt *stmt) {
        if (!stmt) return;

        if ((stmt->type & DECL) == EXPR) {
            // Top-level expression statement — can't replace the pointer from here,
            // but we can fold children in-place.
            foldExpr(static_cast<ast::Expr *>(stmt));
            return;
        }

        switch (stmt->type) {
            case VAR_DECL: {
                auto *d = static_cast<ast::VarDecl *>(stmt);
                if (d->spec.initializer.has_value()) {
                    d->spec.initializer = foldExpr(d->spec.initializer.value());
                }
                break;
            }
            case RETURN_DECL: {
                auto *d = static_cast<ast::ReturnDecl *>(stmt);
                if (d->expr) {
                    d->expr = foldExpr(d->expr);
                }
                break;
            }
            case IF_STMT: {
                auto *s = static_cast<ast::IfStmt *>(stmt);
                if (s->condition) s->condition = foldExpr(s->condition);
                if (s->thenBlock) foldBlock(*s->thenBlock);
                for (auto &branch : s->elseIfs) {
                    if (branch.condition) branch.condition = foldExpr(branch.condition);
                    if (branch.block) foldBlock(*branch.block);
                }
                if (s->elseBlock) foldBlock(*s->elseBlock);
                break;
            }
            case FOR_STMT: {
                auto *s = static_cast<ast::ForStmt *>(stmt);
                if (s->init) foldStmt(s->init);
                if (s->condition) s->condition = foldExpr(s->condition);
                if (s->increment) s->increment = foldExpr(s->increment);
                if (s->body) foldBlock(*s->body);
                break;
            }
            case WHILE_STMT: {
                auto *s = static_cast<ast::WhileStmt *>(stmt);
                if (s->condition) s->condition = foldExpr(s->condition);
                if (s->body) foldBlock(*s->body);
                break;
            }
            default:
                break;
        }
    }

    static void foldBlock(ast::Block &block) {
        for (auto *stmt : block.body) {
            foldStmt(stmt);
        }
    }

    // ------------------------------------------------------------------
    // Public entry point
    // ------------------------------------------------------------------

    void foldConstantsInDecl(ast::Decl *decl) {
        if (!decl) return;

        switch (decl->type) {
            case FUNC_DECL:
            case SHADER_DECL: {
                auto *f = static_cast<ast::FuncDecl *>(decl);
                if (f->block) foldBlock(*f->block);
                break;
            }
            default:
                break;
        }
    }

}
