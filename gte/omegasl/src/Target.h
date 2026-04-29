#ifndef OMEGASL_TARGET_H
#define OMEGASL_TARGET_H

#include <iosfwd>
#include <optional>

#include <omega-common/common.h>

namespace omegasl {

    namespace ast { struct Type; struct CallExpr; }
    struct CodeGen;

    /// Abstract boundary for backend-specific code-generation decisions.
    /// Hooks are added one phase at a time and called from CodeGen / the
    /// surviving *CodeGen subclasses. See
    /// docs/OmegaSL-CodeGen-Target-Refactor-Plan.md.
    struct Target {
        enum Kind { HLSL, MSL, GLSL };
        Kind kind() const { return _kind; }
        virtual ~Target() = default;

        /// Phase 1: spell a (resolved) builtin or user-defined type for this
        /// backend. `pointer` is the source-level pointer flag from the
        /// originating TypeExpr; the target decides the spacing/formatting
        /// of the `*` (HLSL: `T*`, MSL/GLSL: `T *` / `T * `) so that
        /// generated source remains byte-identical to the pre-refactor
        /// output.
        virtual void writeTypeName(ast::Type *t, bool pointer, std::ostream &out) = 0;

        /// Phase 2: spell an OmegaSL attribute (Position, Color, VertexId,
        /// FrontFacing, ...) for this backend. `index` carries the optional
        /// `(N)` from `Color(N)` etc.; HLSL routes it to `SV_TargetN`, MSL
        /// to `color(N)`, GLSL ignores it (the index is consumed elsewhere
        /// in the resource layout). Targets that have no spelling for a
        /// given attribute write nothing — callers should not rely on the
        /// stream being mutated for every input.
        virtual void writeAttribute(OmegaCommon::StrRef name,
                                    std::optional<unsigned> index,
                                    std::ostream &out) = 0;

        /// Phase 3: rewrite an OmegaSL builtin function name to the spelling
        /// the target expects (`lerp` → `mix`, `frac` → `fract`,
        /// `atan2` → `atan`). Targets that do not need a remap return the
        /// input unchanged. The returned StrRef must outlive the call —
        /// in practice it points to either a string literal or the input
        /// itself, both of which the caller already holds live.
        virtual OmegaCommon::StrRef renameBuiltin(OmegaCommon::StrRef name) = 0;

        /// Phase 4: emit a texture sample / read / write call. The target
        /// owns the spelling (HLSL `tex.Sample(s,c)` vs MSL `tex.sample(s,c)`
        /// vs GLSL `texture(samplerND(t,s),c)` etc.) and reaches back into
        /// `cg.generateExpr(arg)` for argument bodies so it does not have
        /// to reimplement the AST walk. `cg.resourceStore` and
        /// `cg.typeResolver` give the target the texture-type lookup it
        /// needs for coord casts. Argument layout follows the OmegaSL
        /// source-level shape: `sample(sampler, texture, coord)`,
        /// `read(texture, coord)`, `write(texture, coord, value)`.
        virtual void emitTextureSample(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) = 0;
        virtual void emitTextureRead(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) = 0;
        virtual void emitTextureWrite(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) = 0;

    protected:
        explicit Target(Kind k) : _kind(k) {}
    private:
        Kind _kind;
    };

    struct HLSLTarget final : Target {
        HLSLTarget();
        ~HLSLTarget() override;
        void writeTypeName(ast::Type *t, bool pointer, std::ostream &out) override;
        void writeAttribute(OmegaCommon::StrRef name,
                            std::optional<unsigned> index,
                            std::ostream &out) override;
        OmegaCommon::StrRef renameBuiltin(OmegaCommon::StrRef name) override;
        void emitTextureSample(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureRead(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureWrite(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
    };

    struct MSLTarget final : Target {
        MSLTarget();
        ~MSLTarget() override;
        void writeTypeName(ast::Type *t, bool pointer, std::ostream &out) override;
        void writeAttribute(OmegaCommon::StrRef name,
                            std::optional<unsigned> index,
                            std::ostream &out) override;
        OmegaCommon::StrRef renameBuiltin(OmegaCommon::StrRef name) override;
        void emitTextureSample(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureRead(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureWrite(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
    };

    struct GLSLTarget final : Target {
        GLSLTarget();
        ~GLSLTarget() override;
        void writeTypeName(ast::Type *t, bool pointer, std::ostream &out) override;
        void writeAttribute(OmegaCommon::StrRef name,
                            std::optional<unsigned> index,
                            std::ostream &out) override;
        OmegaCommon::StrRef renameBuiltin(OmegaCommon::StrRef name) override;
        void emitTextureSample(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureRead(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureWrite(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
    };

}

#endif
