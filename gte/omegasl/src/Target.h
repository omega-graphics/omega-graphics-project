#ifndef OMEGASL_TARGET_H
#define OMEGASL_TARGET_H

#include <iosfwd>
#include <optional>
#include <map>
#include <string>

#include <omega-common/common.h>
#include <omegasl.h>
#include "AST.h"

#ifdef TARGET_VULKAN
#include <shaderc/shaderc.h>
#endif

namespace omegasl {

    struct GLSLCodeOpts;
    struct HLSLCodeOpts;
    struct MetalCodeOpts;

    namespace ast {
        struct Type;
        struct TypeExpr;
        struct CallExpr;
        struct MemberExpr;
        struct ResourceDecl;
        struct ShaderDecl;
        struct StructDecl;
        struct AttributedFieldDecl;
    }
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

        /// Phase 5: small per-statement target hooks.

        /// The keyword that aborts a fragment shader: `"discard"` for
        /// HLSL/GLSL, `"discard_fragment()"` for MSL.
        virtual OmegaCommon::StrRef discardStatement() = 0;

        /// Spell a constructor-style cast `<type>` (without surrounding
        /// parens / argument). Today this is identical to `writeTypeName`
        /// for all three backends — the hook is pinned for future
        /// divergence (e.g. a backend that needs `static_cast<T>(...)`
        /// or a per-type rewrite). `cg` gives access to `typeResolver`
        /// so the target can resolve the source `TypeExpr`.
        virtual void writeCast(CodeGen &cg, ast::TypeExpr *target, std::ostream &out) = 0;

        /// Whether this backend can faithfully emit `&expr` /
        /// `*expr` source-level pointer expressions. HLSL and MSL: yes.
        /// GLSL: no — it has no raw pointer types, so any `&`/`*` is
        /// invalid GLSL. The flag is currently advisory; Sema can
        /// consult it to refuse pointer expressions when targeting GLSL
        /// rather than letting the backend silently emit broken code.
        virtual bool supportsPointerExpr() const = 0;

        /// Phase 7: per-resource binding emission.
        ///
        /// Called once per entry in `_decl->resourceMap` from each
        /// backend's `SHADER_DECL` handler. The target writes its
        /// per-binding source fragment to `out`, fills `layout` with the
        /// numeric layout descriptor that the shared linker serializes
        /// into the omegasllib, and updates its own per-target slot
        /// counters (HLSL uses three: t / u / s; MSL uses three:
        /// buffer / texture / sampler; GLSL uses one: binding).
        ///
        /// `shader` is passed for stage-aware decisions like HLSL's
        /// fragment-stage `space1` vs `space0`. `ioMode` is the
        /// access-direction translation already done by the caller.
        ///
        /// Counter ownership lives entirely inside the target instance
        /// so the shared loop doesn't have to know about per-backend
        /// slot allocation. Call `resetForNextShader()` before the loop.
        virtual void emitResourceBinding(CodeGen &cg,
                                         ast::ResourceDecl *res,
                                         ast::ShaderDecl *shader,
                                         omegasl_shader_layout_desc_io_mode ioMode,
                                         std::ostream &out,
                                         omegasl_shader_layout_desc &layout) = 0;

        /// MSL gathers static-sampler `constexpr sampler ... = sampler(...)`
        /// lines during `emitResourceBinding` (since static samplers do
        /// not appear in the entry-function parameter list). After the
        /// function header is written, the MSL backend invokes this to
        /// flush them into the function body. HLSL/GLSL emit static
        /// samplers inline and have nothing to flush.
        virtual void emitStaticPreamble(std::ostream &out) = 0;

        /// Reset per-target counters (and any gathered preamble such as
        /// MSL's static-sampler list) before iterating the next shader's
        /// resource map. Each backend's `SHADER_DECL` handler must call
        /// this before the shared resource loop.
        virtual void resetForNextShader() = 0;

        /// Phase 8b: emit the per-stage entry-point header — stage
        /// decorators (`[numthreads(...)]`, `[domain(...)]`,
        /// `layout(local_size_x=...)`, the MSL `vertex`/`fragment`/
        /// `kernel` keyword), the return-type / function name / param
        /// list, and any trailing fragment-output suffix. Stops just
        /// before the function body's opening `{` (HLSL/MSL) or after
        /// the synthetic `void main() {` for GLSL.
        ///
        /// `meta` lets the target record stage-specific bookkeeping
        /// (e.g. whether the vertex shader uses VertexID) into the
        /// shaderMap entry without going through a separate hook.
        virtual void emitShaderEntryHeader(CodeGen &cg,
                                           ast::ShaderDecl *decl,
                                           omegasl_shader &meta,
                                           std::ostream &out);

        /// Phase 8c: emit the entry-point body. HLSL/MSL just call
        /// `cg.generateBlock(*decl->block)`. GLSL has a custom loop
        /// with RETURN_DECL rerouting for fragment-output struct
        /// returns and hull/domain `gl_Position` writes — its override
        /// implements that loop directly rather than going through
        /// `generateBlock`. Default delegates to `generateBlock`.
        virtual void emitShaderEntryBody(CodeGen &cg,
                                         ast::ShaderDecl *decl,
                                         omegasl_shader &meta,
                                         std::ostream &out);

        /// Phase 8c: emit anything after the closing `}` of the shader
        /// body. HLSL/MSL: nothing. GLSL: also nothing today (the body
        /// loop emits its own `}`), reserved for future per-target
        /// post-amble.
        virtual void emitShaderEntryFooter(CodeGen &cg,
                                           ast::ShaderDecl *decl,
                                           std::ostream &out);

        /// Whether a user-defined function name would collide with a
        /// stdlib identifier on this backend. Returning true triggers
        /// `osl_user_<name>` mangling at both the definition and call
        /// site for that function. Default: no collision — most user
        /// names pass through unchanged so the generated source stays
        /// readable. Targets override with their stdlib collision sets.
        virtual bool needsMangling(OmegaCommon::StrRef name) const { return false; }

        /// Write an identifier (variable name, function reference, etc.)
        /// to `out`. Default: write raw. GLSL overrides to prefix names
        /// that collide with GLSL keywords (`input`, `output`, `shared`,
        /// `filter`, `common`, `partition`, `active`, `resource`).
        virtual void writeIdentifier(OmegaCommon::StrRef name, std::ostream &out) const {
            out.write(name.data(), name.size());
        }

        /// Emit a member-access expression `lhs.rhs`. Default: walk the
        /// LHS expression then write `.rhs_id` raw — what HLSL/MSL want.
        /// `GLSLTarget` overrides to first consult the
        /// `internalStructVarMap` for fragment-output struct rerouting
        /// (e.g. `out.color = ...` → `_outColor0 = ...`).
        virtual void emitMemberExpr(CodeGen &cg,
                                    ast::MemberExpr *expr,
                                    std::ostream &out);

        /// Phase 9: per-stage source file extension. HLSL: `".hlsl"`.
        /// MSL: `".metal"`. GLSL: `".vert"` / `".frag"` / `".comp"` /
        /// `".tesc"` / `".tese"`. Used by the `*CodeGen` SHADER_DECL
        /// handler when opening the on-disk source file, and by
        /// `compileShader` when constructing the source-path argument
        /// to the offline toolchain. Centralizes the GLSL stage→ext
        /// switch that was previously duplicated.
        virtual const char *shaderFileExt(ast::ShaderDecl::Type stage) const = 0;

        /// Phase 9: invoke the offline toolchain (`dxc` / `metal` /
        /// `glslc`) on the source file at `<srcDir>/<name><shaderFileExt>`,
        /// writing the compiled object to `<outDir>/<name>.<obj-ext>`.
        /// The target owns the per-toolchain command-line shape; the
        /// caller only knows source and output dirs. Returns `false` on
        /// non-zero exit; the target prints its own diagnostic.
        virtual bool compileShader(ast::ShaderDecl::Type stage,
                                   OmegaCommon::StrRef name,
                                   const OmegaCommon::FS::Path &srcDir,
                                   const OmegaCommon::FS::Path &outDir) = 0;

        /// Phase 9: in-process runtime compile. The caller has just
        /// finished the AST walk and passes the captured shader source
        /// (the contents of the `*CodeGen`'s `stringOut` after Phase 10
        /// will fold this onto `CodeGen` itself). The target updates
        /// `meta.data` / `meta.dataSize` (or leaves them null on
        /// failure and prints its own diagnostic). HLSL: `D3DCompile`.
        /// Metal: `compileMTLShader`. GLSL: `shaderc_compile_into_spv`.
        virtual void compileShaderRuntime(ast::ShaderDecl::Type stage,
                                          OmegaCommon::StrRef name,
                                          const std::string &source,
                                          omegasl_shader &meta) = 0;

        /// Phase 9: friendly stage-support gate. Default: every stage
        /// supported. `MSLTarget` overrides to refuse hull/domain (no
        /// Metal tessellation pipeline today — see OmegaSL-Reference.md
        /// bug 3) by returning false and writing a diagnostic to
        /// `diagnosticOut`. The shared SHADER_DECL handler consults
        /// this before opening the output file so the codegen aborts
        /// cleanly without writing partial source.
        virtual bool supportsStage(ast::ShaderDecl::Type stage,
                                   std::string &diagnosticOut) const {
            (void)stage;
            (void)diagnosticOut;
            return true;
        }

    protected:
        explicit Target(Kind k) : _kind(k) {}
    private:
        Kind _kind;
    };

    struct HLSLTarget final : Target {
        explicit HLSLTarget(HLSLCodeOpts &opts);
        ~HLSLTarget() override;
        void writeTypeName(ast::Type *t, bool pointer, std::ostream &out) override;
        void writeAttribute(OmegaCommon::StrRef name,
                            std::optional<unsigned> index,
                            std::ostream &out) override;
        OmegaCommon::StrRef renameBuiltin(OmegaCommon::StrRef name) override;
        void emitTextureSample(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureRead(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureWrite(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        OmegaCommon::StrRef discardStatement() override;
        void writeCast(CodeGen &cg, ast::TypeExpr *target, std::ostream &out) override;
        bool supportsPointerExpr() const override;
        void emitResourceBinding(CodeGen &cg,
                                 ast::ResourceDecl *res,
                                 ast::ShaderDecl *shader,
                                 omegasl_shader_layout_desc_io_mode ioMode,
                                 std::ostream &out,
                                 omegasl_shader_layout_desc &layout) override;
        void emitStaticPreamble(std::ostream &out) override;
        void resetForNextShader() override;
        bool needsMangling(OmegaCommon::StrRef name) const override;
        void emitShaderEntryHeader(CodeGen &cg,
                                   ast::ShaderDecl *decl,
                                   omegasl_shader &meta,
                                   std::ostream &out) override;
        const char *shaderFileExt(ast::ShaderDecl::Type stage) const override;
        bool compileShader(ast::ShaderDecl::Type stage,
                           OmegaCommon::StrRef name,
                           const OmegaCommon::FS::Path &srcDir,
                           const OmegaCommon::FS::Path &outDir) override;
        void compileShaderRuntime(ast::ShaderDecl::Type stage,
                                  OmegaCommon::StrRef name,
                                  const std::string &source,
                                  omegasl_shader &meta) override;
    private:
        HLSLCodeOpts &opts;
        unsigned tResourceCount = 0;
        unsigned uResourceCount = 0;
        unsigned sResourceCount = 0;
    };

    struct MSLTarget final : Target {
        explicit MSLTarget(MetalCodeOpts &opts);
        ~MSLTarget() override;
        void writeTypeName(ast::Type *t, bool pointer, std::ostream &out) override;
        void writeAttribute(OmegaCommon::StrRef name,
                            std::optional<unsigned> index,
                            std::ostream &out) override;
        OmegaCommon::StrRef renameBuiltin(OmegaCommon::StrRef name) override;
        void emitTextureSample(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureRead(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureWrite(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        OmegaCommon::StrRef discardStatement() override;
        void writeCast(CodeGen &cg, ast::TypeExpr *target, std::ostream &out) override;
        bool supportsPointerExpr() const override;
        void emitResourceBinding(CodeGen &cg,
                                 ast::ResourceDecl *res,
                                 ast::ShaderDecl *shader,
                                 omegasl_shader_layout_desc_io_mode ioMode,
                                 std::ostream &out,
                                 omegasl_shader_layout_desc &layout) override;
        void emitStaticPreamble(std::ostream &out) override;
        void resetForNextShader() override;
        bool needsMangling(OmegaCommon::StrRef name) const override;
        /// Phase 8d: MSL owns full shader-entry emission. Header writes
        /// the stage decorator (`vertex`/`fragment`/`kernel`/`[[patch]]
        /// vertex`), return type, name, and the `(...)` parameter list
        /// — resources interleaved with params via the shared
        /// `emitResourcesAndFillLayout`. Body writes the opening `{`,
        /// flushes static-sampler `constexpr sampler ... = sampler(...);`
        /// lines gathered during resource emission, then walks the user
        /// block at indent+1 and closes `}`. The pre-Phase-8d
        /// `MetalCodeGen::shaderDecl` flag is gone.
        void emitShaderEntryHeader(CodeGen &cg,
                                   ast::ShaderDecl *decl,
                                   omegasl_shader &meta,
                                   std::ostream &out) override;
        void emitShaderEntryBody(CodeGen &cg,
                                 ast::ShaderDecl *decl,
                                 omegasl_shader &meta,
                                 std::ostream &out) override;
        const char *shaderFileExt(ast::ShaderDecl::Type stage) const override;
        bool compileShader(ast::ShaderDecl::Type stage,
                           OmegaCommon::StrRef name,
                           const OmegaCommon::FS::Path &srcDir,
                           const OmegaCommon::FS::Path &outDir) override;
        void compileShaderRuntime(ast::ShaderDecl::Type stage,
                                  OmegaCommon::StrRef name,
                                  const std::string &source,
                                  omegasl_shader &meta) override;
        bool supportsStage(ast::ShaderDecl::Type stage,
                           std::string &diagnosticOut) const override;
    private:
        MetalCodeOpts &opts;
        unsigned bufferCount = 0;
        unsigned textureCount = 0;
        unsigned samplerCount = 0;
        /// MSL emits each non-static resource as a comma-separated entry
        /// inside the entry function's parameter list. `paramIndex`
        /// tracks the per-shader iteration count so we know whether to
        /// prepend a comma before the next non-static binding. Resets
        /// in `resetForNextShader`.
        unsigned paramIndex = 0;
        /// Static-sampler `constexpr sampler ... = sampler(...);` lines
        /// gathered during the resource loop, flushed by
        /// `emitStaticPreamble` after the entry function header.
        OmegaCommon::Vector<OmegaCommon::String> staticSamplers;
    };

    struct GLSLTarget final : Target {
        explicit GLSLTarget(GLSLCodeOpts &opts);
        ~GLSLTarget() override;
        void writeTypeName(ast::Type *t, bool pointer, std::ostream &out) override;
        void writeAttribute(OmegaCommon::StrRef name,
                            std::optional<unsigned> index,
                            std::ostream &out) override;
        OmegaCommon::StrRef renameBuiltin(OmegaCommon::StrRef name) override;
        void emitTextureSample(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureRead(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureWrite(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        OmegaCommon::StrRef discardStatement() override;
        void writeCast(CodeGen &cg, ast::TypeExpr *target, std::ostream &out) override;
        bool supportsPointerExpr() const override;
        void emitResourceBinding(CodeGen &cg,
                                 ast::ResourceDecl *res,
                                 ast::ShaderDecl *shader,
                                 omegasl_shader_layout_desc_io_mode ioMode,
                                 std::ostream &out,
                                 omegasl_shader_layout_desc &layout) override;
        void emitStaticPreamble(std::ostream &out) override;
        void resetForNextShader() override;
        bool needsMangling(OmegaCommon::StrRef name) const override;
        void writeIdentifier(OmegaCommon::StrRef name, std::ostream &out) const override;
        void emitMemberExpr(CodeGen &cg, ast::MemberExpr *expr, std::ostream &out) override;
        /// Phase 8c: GLSL owns full shader-entry emission. Header writes
        /// stage decorators, all_used_structs (with fragment-output rerouting),
        /// fragment-output struct decls, file-scope resource bindings,
        /// per-param `layout(location=N) in` lines, attribute-bridge
        /// `extra_stmts`, and `void main()`. Body writes `{` + flushed
        /// `extra_stmts` + custom RETURN_DECL loop + `}`.
        void emitShaderEntryHeader(CodeGen &cg,
                                   ast::ShaderDecl *decl,
                                   omegasl_shader &meta,
                                   std::ostream &out) override;
        void emitShaderEntryBody(CodeGen &cg,
                                 ast::ShaderDecl *decl,
                                 omegasl_shader &meta,
                                 std::ostream &out) override;
        const char *shaderFileExt(ast::ShaderDecl::Type stage) const override;
        bool compileShader(ast::ShaderDecl::Type stage,
                           OmegaCommon::StrRef name,
                           const OmegaCommon::FS::Path &srcDir,
                           const OmegaCommon::FS::Path &outDir) override;
        void compileShaderRuntime(ast::ShaderDecl::Type stage,
                                  OmegaCommon::StrRef name,
                                  const std::string &source,
                                  omegasl_shader &meta) override;

        /// Map a struct field reference to the GLSL identifier that backs it.
        /// `Position` rides on `gl_Position`, indexed `Color(N)` on
        /// `_outColorN`, `Depth` on `gl_FragDepth`, `Coverage` on
        /// `gl_SampleMask[0]`, everything else on a `<struct>_<field>`
        /// varying. Centralizes the routing so the VAR_DECL brace-init,
        /// the MEMBER_EXPR member-access path, and the fragment-output
        /// struct emission all agree on the same identifier.
        void writeInternalFieldRef(const ast::AttributedFieldDecl &field,
                                   const OmegaCommon::String &structName,
                                   std::ostream &out) const;

        /// Fragment-output and internal-struct state. Public so the
        /// GLSL `*CodeGen` can populate these during STRUCT_DECL /
        /// generateDecl processing — `internalStructs` is appended when
        /// an `internal struct` is parsed; `structDeclMap` maps every
        /// struct name to its decl; `generatedStructs` caches the
        /// `struct X { ... };` text emitted for non-internal structs.
        OmegaCommon::Vector<ast::StructDecl *> internalStructs;
        OmegaCommon::Vector<std::pair<OmegaCommon::String, ast::StructDecl *>> internalStructVarMap;
        std::map<OmegaCommon::String, ast::StructDecl *> structDeclMap;
        ast::ShaderDecl::Type currentShaderType = ast::ShaderDecl::Compute;
        OmegaCommon::String activeReturnReplacement;
        ast::StructDecl *fragmentOutputStruct = nullptr;
        OmegaCommon::Map<OmegaCommon::String, OmegaCommon::String> generatedStructs;
        /// Attribute-bridge statements gathered during entry-header param
        /// processing (e.g. `vec3 N = gl_VertexIndex;`). Flushed at the
        /// top of `emitShaderEntryBody` after the opening `{`.
        std::ostringstream extra_stmts;
    private:
        GLSLCodeOpts &opts;
        unsigned binding = 0;
#ifdef TARGET_VULKAN
        /// shaderc compiler used by `compileShaderRuntime`. Initialized
        /// in the constructor and released in the destructor so a
        /// single GLSLTarget owns one shaderc context per session.
        shaderc_compiler_t compiler;
#endif
    };

}

#endif
