#ifndef OMEGASL_TARGET_H
#define OMEGASL_TARGET_H

#include <iosfwd>
#include <optional>
#include <map>
#include <string>

#include <omega-common/utils.h>
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
        struct VarDecl;
        struct ReturnDecl;
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

        /// Phase 2.3: explicit-LOD, LOD-bias, and gradient-based sampling.
        /// Argument layout follows the OmegaSL source-level shape:
        ///   `sampleLOD(sampler, texture, coord, lod)`
        ///   `sampleBias(sampler, texture, coord, bias)`
        ///   `sampleGrad(sampler, texture, coord, ddxArg, ddyArg)`
        /// Sema validates the (sampler, texture, coord) pairing and the
        /// trailing scalar / gradient shape; the target is responsible only
        /// for spelling.
        virtual void emitTextureSampleLOD(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) = 0;
        virtual void emitTextureSampleBias(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) = 0;
        virtual void emitTextureSampleGrad(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) = 0;

        /// Phase 2.3: gather-style sampling. Source-level shape:
        ///   `gather(sampler, texture, coord)`            — defaults to red
        ///   `gather{Red,Green,Blue,Alpha}(sampler, texture, coord)`
        /// `channel` is `-1` for the default `gather` (no explicit channel),
        /// or `0/1/2/3` for `gather{Red,Green,Blue,Alpha}`. Sema restricts
        /// the texture to 2D / 2D-array / cube / cube-array.
        virtual void emitTextureGather(CodeGen &cg, ast::CallExpr *expr, int channel, std::ostream &out) = 0;

        /// §2.3 Phase B — texture query intrinsics.
        ///
        /// `calculateLOD(sampler, texture, coord)` returns the LOD the
        /// hardware would select. Emitted inline as a scalar `float`
        /// expression: HLSL `tex.CalculateLevelOfDetail(s, c)`, MSL
        /// `tex.calculate_clamped_lod(s, c)`, GLSL
        /// `textureQueryLod(samplerND(t, s), c).x` (GLSL returns a vec2; the
        /// `.x` discards the unclamped component — the LOD is documented as
        /// advisory, so the clamped value is fine).
        virtual void emitTextureCalculateLOD(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) = 0;

        /// `getDimensions(texture, lod)` queries mip-level dimensions. Sema
        /// synthesizes the per-rank return shape (`uint` / `uint2` /
        /// `uint3`). MSL and GLSL emit inline (`get_width(...)` etc.
        /// accessors / `uvecN(textureSize(t, int(lod)))`). HLSL's
        /// `GetDimensions` uses out-params and cannot appear as a
        /// sub-expression, so the HLSL backend queues the temp declaration
        /// and the `GetDimensions(...)` call through
        /// `cg.queuePendingStatement(...)` (flushed before the current
        /// statement by `generateBlock`) and emits a `uintN(...)`
        /// constructor over the temporaries inline.
        virtual void emitTextureGetDimensions(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) = 0;

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

        /// §3.7 — emit a user-function parameter, including any
        /// `out`/`inout` qualifier. HLSL/GLSL spell this as a prefix
        /// (`out T name` / `inout T name`); MSL has no `out`/`inout`
        /// keyword and instead requires reference spelling
        /// (`thread T& name`), which the default implementation cannot
        /// produce. Each target implements its own spelling; `param.access`
        /// is the source-level qualifier (`In` is the default and emits
        /// the unqualified type for all three backends).
        virtual void writeFuncParam(CodeGen &cg,
                                    const ast::AttributedFieldDecl &param,
                                    std::ostream &out) = 0;

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

        /// Emit an index expression `lhs[idx]`. Default: walk the LHS,
        /// then `[idx]` raw — what GLSL/MSL want, since their source-level
        /// matrix indexing is column-first (matches OmegaSL's
        /// `Matrix<Ty, col, row>` host convention). `HLSLTarget` overrides
        /// to swap matrix indices (`m[col][row]` → `m[row][col]`) and to
        /// synthesize a column vector for single-level matrix reads, so
        /// the same OmegaSL source produces the same element on every
        /// backend. See OmegaSL-Feature-Gap-Survey §12.1.
        virtual void emitIndexExpr(CodeGen &cg,
                                   ast::IndexExpr *expr,
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
        ///
        /// `requiredFeatures` is the file-scope `#requires(...)` bitfield —
        /// targets that gate command-line flags on declared features
        /// (HLSL bumps to SM 6.2 + `-enable-16bit-types` when FLOAT16 is
        /// declared) read it here. MSL/GLSL ignore it; their
        /// extension/profile decisions live in source.
        virtual bool compileShader(ast::ShaderDecl::Type stage,
                                   OmegaCommon::StrRef name,
                                   uint64_t requiredFeatures,
                                   const OmegaCommon::FS::Path &srcDir,
                                   const OmegaCommon::FS::Path &outDir) = 0;

        /// Phase 9: in-process runtime compile. The caller has just
        /// finished the AST walk and passes the captured shader source
        /// (the contents of the `*CodeGen`'s `stringOut` after Phase 10
        /// will fold this onto `CodeGen` itself). The target updates
        /// `meta.data` / `meta.dataSize` (or leaves them null on
        /// failure and prints its own diagnostic). HLSL: `D3DCompile`.
        /// Metal: `compileMTLShader`. GLSL: `shaderc_compile_into_spv`.
        ///
        /// `requiredFeatures` follows the same contract as the offline
        /// path — targets that gate runtime-compiler flags on declared
        /// features read it here.
        virtual void compileShaderRuntime(ast::ShaderDecl::Type stage,
                                          OmegaCommon::StrRef name,
                                          uint64_t requiredFeatures,
                                          const std::string &source,
                                          omegasl_shader &meta) = 0;

        /// Phase 10: per-target preamble emitted at the top of each
        /// generated shader source file. MSL writes `#include
        /// <metal_stdlib>` + `using namespace metal;`; GLSL writes
        /// `#version 450` + the samplerless-texture extension plus
        /// any feature-driven `#extension` directives needed by the
        /// declared `#requires(...)` set (e.g. FLOAT16 / INT64 →
        /// `GL_EXT_shader_explicit_arithmetic_types_*`). HLSL has
        /// nothing to emit. Default: no-op.
        ///
        /// `cg` is passed so the backend can consult
        /// `cg.fileRequiredFeatures` — the file-scope `#requires` set —
        /// without each backend having to plumb the bitfield through
        /// its constructor.
        virtual void emitDefaultHeaders(CodeGen &/*cg*/, std::ostream &/*out*/) {}

        /// Phase 10: build the per-struct text emitted for a STRUCT_DECL
        /// and stash it in the target's own cache. Each backend has its
        /// own spelling (HLSL `struct X{` no space, MSL `struct X {` with
        /// `[[attribute]]` qualifiers, GLSL plain fields with internal-
        /// struct routing into `internalStructs`). The cached text is
        /// later emitted by `emitShaderUsedStructs` for HLSL/MSL — GLSL
        /// emits its used-struct text inside `emitShaderEntryHeader`.
        virtual void emitStructDecl(CodeGen &cg, ast::StructDecl *decl) = 0;

        /// Phase 10: emit the cached struct definitions used by a shader
        /// at file scope, ahead of the entry header. HLSL emits one
        /// trailing newline per struct, MSL two, GLSL is a no-op (handled
        /// inline by `emitShaderEntryHeader`).
        virtual void emitShaderUsedStructs(CodeGen &cg, ast::ShaderDecl *decl,
                                           std::ostream &out) = 0;

        /// Phase 10: optional hook for VAR_DECL emission. Returning true
        /// means the target handled the entire declaration; the shared
        /// path skips its default emission. Default: false (HLSL/MSL fall
        /// through to the shared form). GLSL overrides to handle internal-
        /// struct vars by decomposing brace initializers into per-field
        /// stores against `gl_Position` / `_outColorN` / etc.
        virtual bool tryEmitVarDecl(CodeGen &/*cg*/, ast::VarDecl */*decl*/) { return false; }

        /// §6.1: emit file-scope thread-group-shared declarations for the
        /// given compute shader. HLSL (`groupshared`) and GLSL (`shared`)
        /// require shared memory at global scope, so they hoist each
        /// top-level `threadgroup` local out of the kernel body here.
        /// Default: no-op — MSL declares them inline inside the kernel.
        virtual void emitThreadgroupGlobals(CodeGen &/*cg*/, ast::ShaderDecl */*decl*/,
                                            std::ostream &/*out*/) {}

        /// Phase 10: optional hook for RETURN_DECL emission. Returning
        /// true means the target handled the entire return; the shared
        /// path skips its default `return [expr]` emission. GLSL overrides
        /// to reroute fragment-output struct returns into bare `return`
        /// (per-field stores happened earlier via member-expr routing) and
        /// to assign to `_outColor` / `gl_Position` for non-struct
        /// fragment / hull / domain returns.
        virtual bool tryEmitReturnDecl(CodeGen &/*cg*/, ast::ReturnDecl */*decl*/) { return false; }

        /// Phase 5.1: optional hook for builtin-call emission. Used when a
        /// builtin doesn't have a single-name equivalent on this backend
        /// and must be rewritten as a different call shape. Returning true
        /// means the target emitted the full `name(args)` form; the shared
        /// path skips its default `<rename(name)>(args)` emission.
        ///
        /// Default returns false. GLSL overrides for `saturate(x)` →
        /// `clamp(x, 0.0, 1.0)` and `fmod(x, y)` → `(x - y * trunc(x / y))`,
        /// neither of which is a built-in GLSL function.
        virtual bool tryEmitBuiltinCall(CodeGen &/*cg*/,
                                        ast::CallExpr */*expr*/,
                                        OmegaCommon::StrRef /*name*/,
                                        std::ostream &/*out*/) { return false; }

        /// Optional hook for a `(lhs op rhs)` binary expression. Returning
        /// true means the target emitted the full sub-expression
        /// (including any wrapping parens it wants); the shared path skips
        /// its default `(<lhs> <op> <rhs>)` emission.
        ///
        /// HLSL overrides for `*` when both sides are matrix/vector — HLSL
        /// requires `mul(a, b)` rather than `(a * b)` for matrix-matrix,
        /// matrix-vector, and vector-matrix products; component-wise
        /// `*` only works on equal-shape operands. MSL and GLSL keep the
        /// infix `*`, which means matrix multiplication on every shape.
        virtual bool tryEmitBinaryExpr(CodeGen &/*cg*/,
                                       ast::BinaryExpr */*expr*/,
                                       std::ostream &/*out*/) { return false; }

        /// Optional hook for a literal expression. Returning true means the
        /// target emitted the full literal (it may wrap the value in a
        /// conversion constructor); the shared path then skips its default
        /// `CodeGen::emitLiteralValue` emission.
        ///
        /// GLSL overrides this: its `GL_EXT_shader_explicit_arithmetic_types`
        /// extension performs no implicit conversion from a default-typed
        /// (float/int) literal into a 16-bit scalar, so a literal whose
        /// Sema-stamped `resolvedType` is `half` / `short` / `ushort` is
        /// wrapped as `float16_t(0.5)` / `int16_t(...)` / `uint16_t(...)`.
        virtual bool tryEmitLiteralExpr(CodeGen &/*cg*/,
                                        ast::LiteralExpr */*expr*/,
                                        std::ostream &/*out*/) { return false; }

        /// §4.3 — the suffix appended to a double-precision floating literal
        /// so the downstream compiler parses a genuine `double` rather than
        /// rounding the value through `float` first. The spelling diverges:
        /// GLSL requires `lf`/`LF` (glslc rejects a bare `l`), while HLSL
        /// requires `l`/`L` (dxc rejects `lf`). Default is `lf` (GLSL); the
        /// HLSL target overrides to `l`. MSL never reaches this — a `double`
        /// shader stubs out, since Metal has no double.
        virtual const char *doubleLiteralSuffix() const { return "lf"; }

        /// Phase 10: per-stage compiled-object file extension recorded in
        /// the shader map. HLSL `.cso`, MSL `.metallib`, GLSL `.spv`.
        /// The shared SHADER_DECL handler uses this to build the entry's
        /// `object_file` key.
        virtual const char *shaderObjectFileExt(ast::ShaderDecl::Type stage) const = 0;

        /// Phase 9: friendly stage-support gate. Default: every stage
        /// supported. `MSLTarget` overrides to refuse hull/domain (no
        /// Metal tessellation pipeline today — see OmegaSL-Reference.md
        /// bug 3) by returning false and writing a diagnostic to
        /// `diagnosticOut`. The shared SHADER_DECL handler consults
        /// this before opening the output file so the codegen aborts
        /// cleanly without writing partial source.
        virtual bool supportsStage(ast::ShaderDecl::Type stage,
                                   std::string &diagnosticOut) const {
            /// Mesh-shader emission is not yet implemented on any backend
            /// (OmegaSL front-end checkpoint). The front-end parses and
            /// type-checks `mesh` stages; per-backend codegen lands in a later
            /// phase, at which point each target overrides this to report mesh
            /// support. Until then a `mesh` stage stubs cleanly here with a
            /// precise reason instead of emitting broken source.
            if(stage == ast::ShaderDecl::Mesh){
                diagnosticOut = "mesh shader codegen is not yet implemented for this backend "
                                "(OmegaSL front-end checkpoint — see gte/docs/Mesh-Shader-Implementation-Plan.md).";
                return false;
            }
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
        /// §4.3 — HLSL/dxc spells the double literal suffix `l` (it rejects
        /// the GLSL `lf`). MSL/GLSL keep the `lf` default.
        const char *doubleLiteralSuffix() const override { return "l"; }
        /// §5.1.0 — takes over `frexp` emission: HLSL's `frexp` writes a
        /// float exponent, but OmegaSL types the exponent out-param as
        /// int/intN, so the float exponent is captured in a temp and cast
        /// back. Statement injection (see `emitTextureGetDimensions`). All
        /// other builtins fall through to `renameBuiltin`.
        bool tryEmitBuiltinCall(CodeGen &cg, ast::CallExpr *expr,
                                OmegaCommon::StrRef name, std::ostream &out) override;
        void emitTextureSample(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureRead(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureWrite(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureSampleLOD(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureSampleBias(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureSampleGrad(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureGather(CodeGen &cg, ast::CallExpr *expr, int channel, std::ostream &out) override;
        void emitTextureCalculateLOD(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureGetDimensions(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        OmegaCommon::StrRef discardStatement() override;
        void writeCast(CodeGen &cg, ast::TypeExpr *target, std::ostream &out) override;
        bool supportsPointerExpr() const override;
        void writeFuncParam(CodeGen &cg,
                            const ast::AttributedFieldDecl &param,
                            std::ostream &out) override;
        /// Prefix identifiers that collide with HLSL reserved words
        /// (`in` / `out` / `inout`, the interpolation modifiers
        /// `linear`/`centroid`/`sample`/etc., the storage qualifiers
        /// `static`/`shared`/`groupshared`, ...). Used both via the
        /// public `writeIdentifier` Target hook (called from the shared
        /// `ID_EXPR` arm and from VAR_DECL / writeFuncParam name
        /// emission) so a definition and every reference end up with
        /// the same `_`-prefixed spelling.
        void writeIdentifier(OmegaCommon::StrRef name, std::ostream &out) const override;
        void emitResourceBinding(CodeGen &cg,
                                 ast::ResourceDecl *res,
                                 ast::ShaderDecl *shader,
                                 omegasl_shader_layout_desc_io_mode ioMode,
                                 std::ostream &out,
                                 omegasl_shader_layout_desc &layout) override;
        void emitStaticPreamble(std::ostream &out) override;
        void resetForNextShader() override;
        void emitShaderEntryHeader(CodeGen &cg,
                                   ast::ShaderDecl *decl,
                                   omegasl_shader &meta,
                                   std::ostream &out) override;
        const char *shaderFileExt(ast::ShaderDecl::Type stage) const override;
        bool compileShader(ast::ShaderDecl::Type stage,
                           OmegaCommon::StrRef name,
                           uint64_t requiredFeatures,
                           const OmegaCommon::FS::Path &srcDir,
                           const OmegaCommon::FS::Path &outDir) override;
        void compileShaderRuntime(ast::ShaderDecl::Type stage,
                                  OmegaCommon::StrRef name,
                                  uint64_t requiredFeatures,
                                  const std::string &source,
                                  omegasl_shader &meta) override;
        void emitStructDecl(CodeGen &cg, ast::StructDecl *decl) override;
        void emitShaderUsedStructs(CodeGen &cg, ast::ShaderDecl *decl, std::ostream &out) override;
        void emitIndexExpr(CodeGen &cg, ast::IndexExpr *expr, std::ostream &out) override;
        bool tryEmitBinaryExpr(CodeGen &cg, ast::BinaryExpr *expr, std::ostream &out) override;
        /// §6.1: hoist each compute-shader `threadgroup` local to file
        /// scope as a `groupshared`-qualified global (HLSL forbids
        /// `groupshared` in function bodies). The body walk skips the
        /// original decl, so no inline `tryEmitVarDecl` hook is needed.
        void emitThreadgroupGlobals(CodeGen &cg, ast::ShaderDecl *decl, std::ostream &out) override;
        const char *shaderObjectFileExt(ast::ShaderDecl::Type stage) const override;

        /// §2b — mesh stage support. Override flips `Mesh` to true for
        /// HLSL: the rest of this target emits SM 6.5 mesh source. The
        /// base `Target::supportsStage` still gates MSL off until 2c.
        bool supportsStage(ast::ShaderDecl::Type stage,
                           std::string &diagnosticOut) const override;

        /// §2b — mesh shaders need a `SetMeshOutputCounts(maxV, maxP);`
        /// auto-emit at the top of the body when the user didn't call
        /// `setMeshOutputs(nv, np)` themselves (the same suppression rule
        /// GLSL uses). Every other stage falls through to the default
        /// `Target::emitShaderEntryBody` block-walk.
        void emitShaderEntryBody(CodeGen &cg,
                                 ast::ShaderDecl *decl,
                                 omegasl_shader &meta,
                                 std::ostream &out) override;
    private:
        HLSLCodeOpts &opts;
        unsigned tResourceCount = 0;
        unsigned uResourceCount = 0;
        unsigned sResourceCount = 0;
        /// §2.4 constant-buffer (`b`) register class for `ConstantBuffer<T>`.
        unsigned bResourceCount = 0;
        OmegaCommon::Map<OmegaCommon::String, OmegaCommon::String> generatedStructs;
        /// §2b — name → StructDecl* index, populated by `emitStructDecl`
        /// alongside the cached HLSL source text. Used by
        /// `emitShaderUsedStructs` to re-emit the mesh-vertex-output
        /// struct with inter-stage semantics (`Color(N) → COLOR<N>`)
        /// instead of the cached form (`Color(N) → SV_Target<N>`, which
        /// is the fragment-output mapping and is not legal as a
        /// mesh-shader vertex-output semantic).
        std::map<OmegaCommon::String, ast::StructDecl *> structDeclMap;
        /// §2b — mesh-stage entry state, mirroring GLSL Phase 2a. Set
        /// by `emitShaderEntryHeader` when entering a `mesh` shader,
        /// consulted by `emitShaderEntryBody` to drive the
        /// `SetMeshOutputCounts(...)` auto-emit, and cleared at body
        /// close. Empty `meshVertsParamName` ⇒ not in a mesh shader.
        OmegaCommon::String meshVertsParamName;
        OmegaCommon::String meshIndicesParamName;
        ast::StructDecl *meshVertsStructDecl = nullptr;
        ast::ShaderDecl::MeshDesc::Topology meshTopology = ast::ShaderDecl::MeshDesc::Triangle;
        unsigned meshMaxVertices = 0;
        unsigned meshMaxPrimitives = 0;
    };

    struct MSLTarget final : Target {
        explicit MSLTarget(MetalCodeOpts &opts);
        ~MSLTarget() override;
        void writeTypeName(ast::Type *t, bool pointer, std::ostream &out) override;
        void writeAttribute(OmegaCommon::StrRef name,
                            std::optional<unsigned> index,
                            std::ostream &out) override;
        OmegaCommon::StrRef renameBuiltin(OmegaCommon::StrRef name) override;
        /// MSL has no `degrees` / `radians` math builtins — they're
        /// absent from `<metal_math>` in every spec rev. This hook
        /// rewrites those calls inline as a multiplication by the
        /// matching π constant; broadcasting handles scalar and vector
        /// arguments uniformly. The texture-side hole (`gradient1d` on
        /// `texture1d`) is handled in `emitTextureSampleGrad`.
        bool tryEmitBuiltinCall(CodeGen &cg,
                                ast::CallExpr *expr,
                                OmegaCommon::StrRef name,
                                std::ostream &out) override;
        void emitTextureSample(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureRead(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureWrite(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureSampleLOD(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureSampleBias(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureSampleGrad(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureGather(CodeGen &cg, ast::CallExpr *expr, int channel, std::ostream &out) override;
        void emitTextureCalculateLOD(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureGetDimensions(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        OmegaCommon::StrRef discardStatement() override;
        void writeCast(CodeGen &cg, ast::TypeExpr *target, std::ostream &out) override;
        bool supportsPointerExpr() const override;
        void writeFuncParam(CodeGen &cg,
                            const ast::AttributedFieldDecl &param,
                            std::ostream &out) override;
        void emitResourceBinding(CodeGen &cg,
                                 ast::ResourceDecl *res,
                                 ast::ShaderDecl *shader,
                                 omegasl_shader_layout_desc_io_mode ioMode,
                                 std::ostream &out,
                                 omegasl_shader_layout_desc &layout) override;
        void emitStaticPreamble(std::ostream &out) override;
        void resetForNextShader() override;
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
                           uint64_t requiredFeatures,
                           const OmegaCommon::FS::Path &srcDir,
                           const OmegaCommon::FS::Path &outDir) override;
        void compileShaderRuntime(ast::ShaderDecl::Type stage,
                                  OmegaCommon::StrRef name,
                                  uint64_t requiredFeatures,
                                  const std::string &source,
                                  omegasl_shader &meta) override;
        /// Stage-support gate. Pre-2c the override only rejected
        /// Hull / Domain (no Metal tessellation pipeline today, per
        /// OmegaSL-Reference.md bug 3) and Mesh ("not implemented
        /// yet"). §2c keeps the Hull/Domain rejection and flips Mesh
        /// to supported — the rest of this target now emits MSL mesh
        /// source (`[[mesh]]`, `mesh<V, void, MaxV, MaxP,
        /// topology::X>` handle, scratch array + flush loop pattern
        /// documented in gte/docs/Mesh-Shader-Implementation-Plan.md
        /// → Phase 2c).
        bool supportsStage(ast::ShaderDecl::Type stage,
                           std::string &diagnosticOut) const override;
        void emitDefaultHeaders(CodeGen &cg, std::ostream &out) override;
        void emitStructDecl(CodeGen &cg, ast::StructDecl *decl) override;
        void emitShaderUsedStructs(CodeGen &cg, ast::ShaderDecl *decl, std::ostream &out) override;
        /// §6.1: emit a `threadgroup`-qualified local inline at kernel
        /// scope (MSL declares shared memory inside the kernel, not at
        /// file scope). Returns true to suppress the shared emission.
        bool tryEmitVarDecl(CodeGen &cg, ast::VarDecl *decl) override;
        const char *shaderObjectFileExt(ast::ShaderDecl::Type stage) const override;

        /// §2c — mesh `out vertices` slot access:
        /// `verts[i].field = expr` rewrites to
        /// `__omegasl_verts_scratch[i].field = expr`. MSL's `mesh<...>`
        /// handle has no per-field accessor — only `set_vertex(i, T)`
        /// — so the user's per-field writes accumulate into a scratch
        /// array; one flush loop at body end calls `set_vertex` for
        /// every slot. Empty `meshVertsParamName` ⇒ "not a mesh
        /// shader" ⇒ default `lhs.field` emission falls through.
        void emitMemberExpr(CodeGen &cg, ast::MemberExpr *expr, std::ostream &out) override;

        /// §2c — mesh `out indices` slot write expansion. MSL's
        /// `set_index(slot, vertexIdx)` is per-slot (slot = i*K + k for
        /// K-wide topology), but OmegaSL writes a whole tuple per
        /// primitive: `tris[i] = uintK(a, b, c);`. Detect `=` whose lhs
        /// is `INDEX_EXPR[ id == meshIndicesParamName ]` and expand
        /// into a `uintK` temp + K `set_index` calls via the shared
        /// pending-statement queue. Returns true to suppress the
        /// default `lhs = rhs` emission. Falls through for any other
        /// `=` (and for non-`=` ops).
        bool tryEmitBinaryExpr(CodeGen &cg, ast::BinaryExpr *expr, std::ostream &out) override;

        /// §16 — hull-kernel `return` rewrite. A hull lowers to a compute
        /// kernel: its per-control-point `return <expr>;` is not a real
        /// return but a store into the per-CP output buffer
        /// (`<hullOut>[<vid>] = <expr>`). Active only inside a hull shader
        /// (`inHullShader`); every other stage falls through to the default
        /// `return <expr>` emission.
        bool tryEmitReturnDecl(CodeGen &cg, ast::ReturnDecl *decl) override;
    private:
        MetalCodeOpts &opts;
        std::map<std::string, std::string> generatedStructs;
        /// §2c — name → StructDecl* index, populated by `emitStructDecl`
        /// alongside the cached MSL source text. Used by
        /// `emitShaderUsedStructs` to re-emit the mesh-vertex-output
        /// struct with inter-stage semantics (strip `[[color(N)]]` /
        /// `[[texcoord(N)]]` — those are fragment-output attributes,
        /// not legal as mesh-vertex-output decorations; `[[position]]`
        /// is preserved). Same pattern as HLSL Phase 2b.
        std::map<std::string, ast::StructDecl *> structDeclMap;
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
        /// §2c — mesh-stage entry state, mirroring GLSL 2a / HLSL 2b.
        /// Set by `emitShaderEntryHeader` when entering a `mesh`
        /// shader, consulted by `emitShaderEntryBody` (scratch decl +
        /// set_primitive_count auto-emit + flush loop) and
        /// `emitMemberExpr` / `tryEmitBinaryExpr` (the lvalue
        /// rewrites). Empty `meshVertsParamName` ⇒ not in a mesh shader.
        OmegaCommon::String meshVertsParamName;
        OmegaCommon::String meshIndicesParamName;
        ast::StructDecl *meshVertsStructDecl = nullptr;
        ast::ShaderDecl::MeshDesc::Topology meshTopology = ast::ShaderDecl::MeshDesc::Triangle;
        unsigned meshMaxVertices = 0;
        unsigned meshMaxPrimitives = 0;

        /// §16 — tessellation hull-kernel state. Set by
        /// `emitShaderEntryHeader` for a `hull` shader, consumed by
        /// `tryEmitReturnDecl` (return→store) and `emitShaderEntryBody`
        /// (the tess-factor epilogue). `inHullShader == false` ⇒ not a hull
        /// ⇒ every hook falls through to its normal path.
        bool inHullShader = false;
        OmegaCommon::String hullOutBufferName;   /// the single `out` buffer (store target)
        OmegaCommon::String hullVidName;         /// the VertexID param = global control-point index
        OmegaCommon::String hullPatchFnName;     /// patchfn called once per patch for the factors
        unsigned hullControlPoints = 3;          /// N — per-patch output control-point count
        bool hullDomainIsTri = true;             /// tri (3 edge + 1 inside) vs quad (4 + 2)
        /// Factor-struct field names + array-ness, resolved from the
        /// patchfn's return struct so the epilogue can spell
        /// `__pc.<field>[i]`. A scalar factor field is written with no
        /// subscript.
        OmegaCommon::String hullEdgeFieldName;
        OmegaCommon::String hullInsideFieldName;
        bool hullEdgeIsArray = false;
        bool hullInsideIsArray = false;
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
        void emitTextureSampleLOD(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureSampleBias(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureSampleGrad(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureGather(CodeGen &cg, ast::CallExpr *expr, int channel, std::ostream &out) override;
        void emitTextureCalculateLOD(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        void emitTextureGetDimensions(CodeGen &cg, ast::CallExpr *expr, std::ostream &out) override;
        OmegaCommon::StrRef discardStatement() override;
        void writeCast(CodeGen &cg, ast::TypeExpr *target, std::ostream &out) override;
        bool supportsPointerExpr() const override;
        void writeFuncParam(CodeGen &cg,
                            const ast::AttributedFieldDecl &param,
                            std::ostream &out) override;
        void emitResourceBinding(CodeGen &cg,
                                 ast::ResourceDecl *res,
                                 ast::ShaderDecl *shader,
                                 omegasl_shader_layout_desc_io_mode ioMode,
                                 std::ostream &out,
                                 omegasl_shader_layout_desc &layout) override;
        void emitStaticPreamble(std::ostream &out) override;
        void resetForNextShader() override;
        void writeIdentifier(OmegaCommon::StrRef name, std::ostream &out) const override;
        void emitMemberExpr(CodeGen &cg, ast::MemberExpr *expr, std::ostream &out) override;
        bool tryEmitBuiltinCall(CodeGen &cg,
                                ast::CallExpr *expr,
                                OmegaCommon::StrRef name,
                                std::ostream &out) override;
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
                           uint64_t requiredFeatures,
                           const OmegaCommon::FS::Path &srcDir,
                           const OmegaCommon::FS::Path &outDir) override;
        void compileShaderRuntime(ast::ShaderDecl::Type stage,
                                  OmegaCommon::StrRef name,
                                  uint64_t requiredFeatures,
                                  const std::string &source,
                                  omegasl_shader &meta) override;
        void emitDefaultHeaders(CodeGen &cg, std::ostream &out) override;
        void emitStructDecl(CodeGen &cg, ast::StructDecl *decl) override;
        void emitShaderUsedStructs(CodeGen &cg, ast::ShaderDecl *decl, std::ostream &out) override;
        bool tryEmitVarDecl(CodeGen &cg, ast::VarDecl *decl) override;
        bool tryEmitReturnDecl(CodeGen &cg, ast::ReturnDecl *decl) override;
        /// §4.1: wrap a literal coerced into a 16-bit scalar slot in the
        /// target-type constructor — GLSL's explicit-arithmetic-types
        /// extension does no implicit float→float16_t / int→int16_t cast.
        bool tryEmitLiteralExpr(CodeGen &cg, ast::LiteralExpr *expr, std::ostream &out) override;
        /// §6.1: hoist each compute-shader `threadgroup` local to file
        /// scope as a `shared`-qualified global (GLSL forbids `shared` in
        /// function bodies).
        void emitThreadgroupGlobals(CodeGen &cg, ast::ShaderDecl *decl, std::ostream &out) override;
        const char *shaderObjectFileExt(ast::ShaderDecl::Type stage) const override;

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

        /// §2a — mesh stage support. Override flips `Mesh` to true for
        /// GLSL: the rest of this target emits `GL_EXT_mesh_shader`
        /// source. The base `Target::supportsStage` still gates every
        /// other backend off until its Phase-2 codegen lands.
        bool supportsStage(ast::ShaderDecl::Type stage,
                           std::string &diagnosticOut) const override;

        /// §2a — mesh-stage index re-routing. When the lhs of an index
        /// expression is the identifier of the entry's `out indices`
        /// parameter, emit the per-topology `gl_Primitive{Triangle,Line,
        /// Point}IndicesEXT[idx]` builtin instead of `name[idx]`. Every
        /// other index expression falls through to the default
        /// `lhs[idx]` emission inherited from `Target::emitIndexExpr`.
        void emitIndexExpr(CodeGen &cg, ast::IndexExpr *expr, std::ostream &out) override;

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

        /// §2a — mesh-stage output-routing state. Populated by
        /// `emitShaderEntryHeader` when entering a `mesh` shader,
        /// consulted by `emitMemberExpr` (verts[i].field) and
        /// `emitIndexExpr` (tris[i]) to retarget those writes onto the
        /// `GL_EXT_mesh_shader` builtins, and cleared at the end of
        /// `emitShaderEntryBody`. Empty `meshVertsParamName` /
        /// `meshIndicesParamName` ⇒ "not in a mesh shader," which is
        /// what every non-mesh entry sees.
        OmegaCommon::String meshVertsParamName;
        OmegaCommon::String meshIndicesParamName;
        ast::StructDecl *meshVertsStructDecl = nullptr;
        ast::ShaderDecl::MeshDesc::Topology meshTopology = ast::ShaderDecl::MeshDesc::Triangle;
        unsigned meshMaxVertices = 0;
        unsigned meshMaxPrimitives = 0;
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
