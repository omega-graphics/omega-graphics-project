#ifndef OMEGASL_CONSTFOLD_H
#define OMEGASL_CONSTFOLD_H

#include "AST.h"

namespace omegasl {

    /// @brief Recursively folds constant expressions in the AST.
    /// Replaces binary/unary operations on numeric literals with their
    /// evaluated result (e.g. `2.0 * 3.14159` becomes `6.28318`).
    /// Call after semantic analysis, before code generation.
    void foldConstantsInDecl(ast::Decl *decl);

}

#endif
