#include "Target.h"
#include "AST.h"
#include "CodeGen.h"
#include <ostream>
#include <sstream>
#include <unordered_set>
#include <string>
#include <omega-common/multithread.h>

#ifdef TARGET_DIRECTX
#    include <d3dcompiler.h>
#    pragma comment(lib, "d3dcompiler.lib")
#endif

#define HLSL_TEXTURE1D            "Texture1D"
#define HLSL_RW_TEXTURE1D         "RWTexture1D"
#define HLSL_TEXTURE2D            "Texture2D"
#define HLSL_RW_TEXTURE2D         "RWTexture2D"
#define HLSL_TEXTURE3D            "Texture3D"
#define HLSL_RW_TEXTURE3D         "RWTexture3D"
#define HLSL_TEXTURE1D_ARRAY      "Texture1DArray"
#define HLSL_RW_TEXTURE1D_ARRAY   "RWTexture1DArray"
#define HLSL_TEXTURE2D_ARRAY      "Texture2DArray"
#define HLSL_RW_TEXTURE2D_ARRAY   "RWTexture2DArray"
#define HLSL_TEXTURECUBE          "TextureCube"
#define HLSL_TEXTURECUBE_ARRAY    "TextureCubeArray"
#define HLSL_TEXTURE2D_MS         "Texture2DMS"
#define HLSL_TEXTURE2D_MS_ARRAY   "Texture2DMSArray"
#define HLSL_BUFFER               "StructuredBuffer"
#define HLSL_RW_BUFFER            "RWStructuredBuffer"
#define HLSL_CONSTANT_BUFFER      "ConstantBuffer"
#define HLSL_SAMPLER              "SamplerState"

namespace omegasl {

    HLSLTarget::HLSLTarget(HLSLCodeOpts &opts) : Target(Target::HLSL), opts(opts) {}
    HLSLTarget::~HLSLTarget() = default;

    const char *HLSLTarget::shaderFileExt(ast::ShaderDecl::Type /*stage*/) const {
        return ".hlsl";
    }

    bool HLSLTarget::compileShader(ast::ShaderDecl::Type stage,
                                   OmegaCommon::StrRef name,
                                   uint64_t requiredFeatures,
                                   const OmegaCommon::FS::Path &srcDir,
                                   const OmegaCommon::FS::Path &outDir) {
        /// §4.1 — `float16_t` / `int16_t` / `uint16_t` are gated behind
        /// SM 6.2 + `-enable-16bit-types`. When the file declares
        /// `#requires(FLOAT16)` we bump the target profile and pass the
        /// flag; otherwise we stay on the SM 5.0 default that historic
        /// shaders compile against. SM 6.2 is the lowest profile that
        /// accepts the 16-bit family, and dxc happily downgrades to
        /// older feature levels at runtime if the actual device cap is
        /// lower — the runtime gate (`OMEGASL_FEATURE_BIT_FLOAT16`) is
        /// what protects callers on hardware that doesn't support it.
        const bool needs16Bit = (requiredFeatures & OMEGASL_FEATURE_BIT_FLOAT16) != 0;

        std::ostringstream out;
        out << " -nologo -T";
        const char *profileTag = needs16Bit ? "_6_2" : "_5_0";
        if (stage == ast::ShaderDecl::Vertex) {
            out << "vs" << profileTag;
        } else if (stage == ast::ShaderDecl::Fragment) {
            out << "ps" << profileTag;
        } else if (stage == ast::ShaderDecl::Compute) {
            out << "cs" << profileTag;
        } else if (stage == ast::ShaderDecl::Hull) {
            out << "hs" << profileTag;
        } else if (stage == ast::ShaderDecl::Domain) {
            out << "ds" << profileTag;
        }
        out << " -E" << name.data() << " -Fo "
            << OmegaCommon::FS::Path(outDir).append(name).concat(".cso").absPath();
        /// `/Zpc` locks matrix packing to column-major. OmegaSL stores
        /// matrices column-major across host + GLSL + MSL, and the
        /// runtime path passes `D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR` to
        /// `D3DCompile`. This explicit flag stops the offline path from
        /// silently disagreeing if a future dxc default ever flips. See
        /// OmegaSL-Feature-Gap-Survey §12.2.
        out << " " << OmegaCommon::FS::Path(srcDir).append(name).concat(shaderFileExt(stage)).absPath() << " /Zi /Zpc";
        if (needs16Bit) {
            out << " -enable-16bit-types";
        }

        auto dxc_process = OmegaCommon::ChildProcess::OpenWithStdoutPipe(opts.dxc_cmd, out.str().c_str());
        auto res = dxc_process.wait();

        if (res != 0) {
            std::cerr << "error: dxc failed (exit " << res << ") for shader '" << name.data() << "'" << std::endl;
            return false;
        }
        return true;
    }

    void HLSLTarget::compileShaderRuntime(ast::ShaderDecl::Type stage,
                                          OmegaCommon::StrRef name,
                                          uint64_t requiredFeatures,
                                          const std::string &source,
                                          omegasl_shader &meta) {
        /// §4.1 — `D3DCompile` is the legacy FXC interface and tops out at
        /// SM 5.1; the 16-bit type family needs SM 6.2 + dxcompiler. If a
        /// shader gated on FLOAT16 reaches this path, fail loud rather
        /// than silently compile and emit garbage. Migrating the runtime
        /// to dxc is the long-term fix.
        if ((requiredFeatures & OMEGASL_FEATURE_BIT_FLOAT16) != 0) {
            std::cerr << "error: runtime D3DCompile path cannot compile '" << name.data()
                      << "' — `#requires(FLOAT16)` needs SM 6.2 (dxc), but the runtime "
                         "uses D3DCompile (SM 5.1 max). Use the offline pipeline or "
                         "switch the runtime to dxc." << std::endl;
            return;
        }
#ifdef TARGET_DIRECTX
        std::cout << "[OmegaSL HLSL] Compiling shader '" << name.data() << "' target="
                  << (stage == ast::ShaderDecl::Vertex     ? "vs"
                      : stage == ast::ShaderDecl::Fragment ? "ps"
                                                          : "cs")
                  << "\n"
                  << source << std::endl;
        ID3DBlob *blob = nullptr;
        OmegaCommon::String target;
        if (stage == ast::ShaderDecl::Vertex) {
            target = "vs_5_1";
        } else if (stage == ast::ShaderDecl::Fragment) {
            target = "ps_5_1";
        } else if (stage == ast::ShaderDecl::Compute) {
            target = "cs_5_1";
        }

        ID3DBlob *errorBlob = nullptr;

        /// `D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR` makes HLSL's storage
        /// layout match the host's `OmegaCommon::Matrix<Ty, col, row>`
        /// (column-major) byte-for-byte, so a matrix `memcpy`'d into a
        /// buffer reads correctly without a runtime transpose. Pairs
        /// with the `column_major` qualifier emitted on every matrix-
        /// typed field in `writeTypeName` and the offline `/Zpc` flag.
        /// See OmegaSL-Feature-Gap-Survey §12.2.
        D3DCompile(source.data(), source.size(), name.data(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                   name.data(), target.data(),
                   D3DCOMPILE_DEBUG | D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR,
                   NULL, &blob, &errorBlob);

        if (errorBlob != nullptr) {
            std::cout << "OMEGASL COMPILE ERROR: D3D ERROR:" << (char *)errorBlob->GetBufferPointer() << std::endl;
        }

        if (blob != nullptr) {
            meta.data = blob->GetBufferPointer();
            meta.dataSize = blob->GetBufferSize();
        }
#else
        (void)stage;
        (void)name;
        (void)source;
        (void)meta;
#endif
    }

    /// Curated set of HLSL stdlib identifiers that a user function name
    /// would shadow. Names listed here trigger `osl_user_<name>` mangling
    /// at both definition and call sites. Names not in this set pass
    /// through unchanged so the generated source stays readable. Add
    /// entries when a real collision is observed.
    bool HLSLTarget::needsMangling(OmegaCommon::StrRef name) const {
        static const std::unordered_set<std::string> hlslStdlib = {
            "abs","acos","all","any","asin","atan","atan2","ceil","clamp",
            "clip","cos","cosh","cross","ddx","ddy","degrees","determinant",
            "distance","dot","exp","exp2","floor","fmod","frac","frexp",
            "fwidth","isfinite","isinf","isnan","ldexp","length","lerp","lit",
            "log","log10","log2","mad","max","min","modf","mul","normalize",
            "pow","radians","reflect","refract","round","rsqrt","saturate",
            "sign","sin","sinh","smoothstep","sqrt","step","tan","tanh",
            "transpose","trunc"
        };
        return hlslStdlib.count(std::string(name)) > 0;
    }

    void HLSLTarget::resetForNextShader() {
        tResourceCount = 0;
        uResourceCount = 0;
        sResourceCount = 0;
        bResourceCount = 0;
    }

    void HLSLTarget::emitStaticPreamble(std::ostream &/*out*/) {
        /// HLSL emits static samplers inline as `: register(sN, spaceM);`
        /// during emitResourceBinding — no separate preamble.
    }

    void HLSLTarget::emitResourceBinding(CodeGen &cg,
                                         ast::ResourceDecl *res_desc,
                                         ast::ShaderDecl *shader,
                                         omegasl_shader_layout_desc_io_mode ioMode,
                                         std::ostream &out,
                                         omegasl_shader_layout_desc &layoutDesc) {
        layoutDesc.offset = 0;
        layoutDesc.io_mode = ioMode;

        auto _t = cg.typeResolver->resolveTypeWithExpr(res_desc->typeExpr);

        bool isTResource = false, isSResource = false, isBResource = false;

        if (_t == ast::builtins::buffer_type) {
            layoutDesc.type = OMEGASL_SHADER_BUFFER_DESC;
            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                isTResource = true;
                out << HLSL_BUFFER;
            } else {
                out << HLSL_RW_BUFFER;
            }
            out << "<";
            writeTypeName(cg.typeResolver->resolveTypeWithExpr(res_desc->typeExpr->args[0]),
                          res_desc->typeExpr->args[0]->pointer, out);
            out << ">";
        } else if (_t == ast::builtins::uniform_type) {
            /// §2.4 constant buffer. Read-only (Sema enforces `In` access),
            /// accessed as `name.field`. `ConstantBuffer<T>` binds to a `b`
            /// register and requires T to be a struct (Sema enforces).
            layoutDesc.type = OMEGASL_SHADER_UNIFORM_DESC;
            isBResource = true;
            out << HLSL_CONSTANT_BUFFER << "<";
            writeTypeName(cg.typeResolver->resolveTypeWithExpr(res_desc->typeExpr->args[0]),
                          res_desc->typeExpr->args[0]->pointer, out);
            out << ">";
        } else if (_t == ast::builtins::texture1d_type) {
            layoutDesc.type = OMEGASL_SHADER_TEXTURE1D_DESC;
            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                isTResource = true;
                out << HLSL_TEXTURE1D;
            } else {
                out << HLSL_RW_TEXTURE1D;
            }
            out << "<float4>";
        } else if (_t == ast::builtins::texture2d_type) {
            layoutDesc.type = OMEGASL_SHADER_TEXTURE2D_DESC;
            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                isTResource = true;
                out << HLSL_TEXTURE2D;
            } else {
                out << HLSL_RW_TEXTURE2D;
            }
            out << "<float4>";
        } else if (_t == ast::builtins::texture3d_type) {
            layoutDesc.type = OMEGASL_SHADER_TEXTURE3D_DESC;
            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                isTResource = true;
                out << HLSL_TEXTURE3D;
            } else {
                out << HLSL_RW_TEXTURE3D;
            }
            out << "<float4>";
        } else if (_t == ast::builtins::texture1d_array_type) {
            layoutDesc.type = OMEGASL_SHADER_TEXTURE1D_ARRAY_DESC;
            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                isTResource = true;
                out << HLSL_TEXTURE1D_ARRAY;
            } else {
                out << HLSL_RW_TEXTURE1D_ARRAY;
            }
            out << "<float4>";
        } else if (_t == ast::builtins::texture2d_array_type) {
            layoutDesc.type = OMEGASL_SHADER_TEXTURE2D_ARRAY_DESC;
            if (ioMode == OMEGASL_SHADER_DESC_IO_IN) {
                isTResource = true;
                out << HLSL_TEXTURE2D_ARRAY;
            } else {
                out << HLSL_RW_TEXTURE2D_ARRAY;
            }
            out << "<float4>";
        } else if (_t == ast::builtins::texturecube_type) {
            /// HLSL `TextureCube` is read-only; UAV cube views are expressed
            /// via `RWTexture2DArray` aliasing on the runtime side. Sema
            /// already rejects `write` to cube textures, so we always emit
            /// the read-only form here.
            layoutDesc.type = OMEGASL_SHADER_TEXTURECUBE_DESC;
            isTResource = true;
            out << HLSL_TEXTURECUBE << "<float4>";
        } else if (_t == ast::builtins::texturecube_array_type) {
            layoutDesc.type = OMEGASL_SHADER_TEXTURECUBE_ARRAY_DESC;
            isTResource = true;
            out << HLSL_TEXTURECUBE_ARRAY << "<float4>";
        } else if (_t == ast::builtins::texture2d_ms_type) {
            /// MS textures are read-only; Sema already rejects `write`.
            layoutDesc.type = OMEGASL_SHADER_TEXTURE2D_MS_DESC;
            isTResource = true;
            out << HLSL_TEXTURE2D_MS << "<float4>";
        } else if (_t == ast::builtins::texture2d_ms_array_type) {
            layoutDesc.type = OMEGASL_SHADER_TEXTURE2D_MS_ARRAY_DESC;
            isTResource = true;
            out << HLSL_TEXTURE2D_MS_ARRAY << "<float4>";
        } else if (_t == ast::builtins::sampler1d_type) {
            isSResource = true;
            layoutDesc.type = res_desc->isStatic ? OMEGASL_SHADER_STATIC_SAMPLER1D_DESC
                                                 : OMEGASL_SHADER_SAMPLER1D_DESC;
            out << HLSL_SAMPLER;
        } else if (_t == ast::builtins::sampler2d_type) {
            isSResource = true;
            layoutDesc.type = res_desc->isStatic ? OMEGASL_SHADER_STATIC_SAMPLER2D_DESC
                                                 : OMEGASL_SHADER_SAMPLER2D_DESC;
            out << HLSL_SAMPLER;
        } else if (_t == ast::builtins::sampler3d_type) {
            isSResource = true;
            layoutDesc.type = res_desc->isStatic ? OMEGASL_SHADER_STATIC_SAMPLER3D_DESC
                                                 : OMEGASL_SHADER_SAMPLER3D_DESC;
            out << HLSL_SAMPLER;
        } else if (_t == ast::builtins::samplercube_type) {
            isSResource = true;
            layoutDesc.type = res_desc->isStatic ? OMEGASL_SHADER_STATIC_SAMPLERCUBE_DESC
                                                 : OMEGASL_SHADER_SAMPLERCUBE_DESC;
            out << HLSL_SAMPLER;
        }

        out << " " << res_desc->name;

        auto fragmentSpace = [&]() -> const char * {
            return shader->shaderType == ast::ShaderDecl::Fragment ? "1" : "0";
        };

        if (isSResource && res_desc->isStatic) {
            /// Static-sampler descriptor is serialized into the layout
            /// metadata alongside the `: register(sN, spaceM);` line.
            layoutDesc.sampler_desc.filter = res_desc->staticSamplerDesc->filter;
            layoutDesc.sampler_desc.u_address_mode = res_desc->staticSamplerDesc->uAddressMode;
            layoutDesc.sampler_desc.v_address_mode = res_desc->staticSamplerDesc->vAddressMode;
            layoutDesc.sampler_desc.w_address_mode = res_desc->staticSamplerDesc->wAddressMode;
            layoutDesc.sampler_desc.max_anisotropy = res_desc->staticSamplerDesc->maxAnisotropy;
            layoutDesc.location = res_desc->registerNumber;
            out << ": register(s" << sResourceCount << ",space" << fragmentSpace() << ");" << std::endl;
            sResourceCount += 1;
        }

        if (!res_desc->isStatic) {
            out << ": register(";
            layoutDesc.location = res_desc->registerNumber;
            if (isTResource) {
                out << "t" << tResourceCount;
                layoutDesc.gpu_relative_loc = tResourceCount;
                ++tResourceCount;
            } else if (isSResource) {
                out << "s" << sResourceCount;
                layoutDesc.gpu_relative_loc = sResourceCount;
                ++sResourceCount;
            } else if (isBResource) {
                out << "b" << bResourceCount;
                layoutDesc.gpu_relative_loc = bResourceCount;
                ++bResourceCount;
            } else {
                out << "u" << uResourceCount;
                layoutDesc.gpu_relative_loc = uResourceCount;
                ++uResourceCount;
            }
            out << ",space" << fragmentSpace() << ");" << std::endl;
        }
    }

    OmegaCommon::StrRef HLSLTarget::discardStatement() { return "discard"; }

    void HLSLTarget::writeCast(CodeGen &cg, ast::TypeExpr *t, std::ostream &out) {
        writeTypeName(cg.typeResolver->resolveTypeWithExpr(t), t->pointer, out);
    }

    /// §3.7 — HLSL spells `out` / `inout` as a prefix keyword on the
    /// parameter. `in` is the default and is left implicit so the
    /// generated source stays byte-identical to the pre-3.7 output for
    /// unqualified params.
    void HLSLTarget::writeFuncParam(CodeGen &cg,
                                    const ast::AttributedFieldDecl &param,
                                    std::ostream &out) {
        if (param.access == ast::AttributedFieldDecl::Out) {
            out << "out ";
        } else if (param.access == ast::AttributedFieldDecl::Inout) {
            out << "inout ";
        }
        writeTypeName(cg.typeResolver->resolveTypeWithExpr(param.typeExpr),
                      param.typeExpr->pointer, out);
        out << " ";
        writeIdentifier(param.name, out);
    }

    /// HLSL reserves a wider keyword set than the C-family backends —
    /// the interpolation modifiers (`linear`, `centroid`, `sample`,
    /// `nointerpolation`, `noperspective`), the storage qualifiers
    /// (`static`, `shared`, `groupshared`, `precise`), and the param
    /// access modifiers (`in`, `out`, `inout`) all collide with names
    /// that are perfectly legal OmegaSL identifiers. When a user shader
    /// names a variable or parameter `out`, dxc would otherwise refuse
    /// the source. Mirror what GLSL does for its own reserved set:
    /// prefix with `_` at every emit site that goes through the hook
    /// (`ID_EXPR`, `VAR_DECL` name, `writeFuncParam` name).
    /// HLSL's `*` is component-wise and only accepts equal-shape operands.
    /// Matrix×matrix, matrix×vector, and vector×matrix products require
    /// `mul(a, b)`. MSL/GLSL keep `*` for all three shapes (it's defined
    /// as matrix multiplication on those backends), so the rewrite stays
    /// HLSL-local. Scalar cases — scalar×matrix, scalar×vector,
    /// component-wise vector×vector — pass through with infix `*`.
    ///
    /// We read `resolvedType` (stamped by Sema during BINARY_EXPR
    /// evaluation) rather than calling `evalExprForTypeExpr` here —
    /// `evalExprForTypeExpr` re-runs Sema, but at codegen time the
    /// per-function `variableMap` has been cleared, so any ID_EXPR
    /// inside `expr->lhs` / `expr->rhs` would emit a spurious
    /// "undeclared identifier" diagnostic. `resolveTypeWithExpr` is a
    /// pure name lookup against the static type table and safe to call.
    bool HLSLTarget::tryEmitBinaryExpr(CodeGen &cg, ast::BinaryExpr *expr, std::ostream &out) {
        if (expr->op != "*") return false;
        auto *lhsTyExpr = expr->lhs->resolvedType;
        auto *rhsTyExpr = expr->rhs->resolvedType;
        if (lhsTyExpr == nullptr || rhsTyExpr == nullptr) return false;
        auto *lhsT = cg.typeResolver->resolveTypeWithExpr(lhsTyExpr);
        auto *rhsT = cg.typeResolver->resolveTypeWithExpr(rhsTyExpr);
        auto isMatrix = [](ast::Type *t) {
            return t == ast::builtins::float2x2_type
                || t == ast::builtins::float3x3_type
                || t == ast::builtins::float4x4_type
                || t == ast::builtins::float2x3_type
                || t == ast::builtins::float2x4_type
                || t == ast::builtins::float3x2_type
                || t == ast::builtins::float3x4_type
                || t == ast::builtins::float4x2_type
                || t == ast::builtins::float4x3_type;
        };
        auto isFloatVec = [](ast::Type *t) {
            return t == ast::builtins::float2_type
                || t == ast::builtins::float3_type
                || t == ast::builtins::float4_type;
        };
        const bool needsMul =
            (isMatrix(lhsT) && isMatrix(rhsT))
            || (isMatrix(lhsT) && isFloatVec(rhsT))
            || (isFloatVec(lhsT) && isMatrix(rhsT));
        if (!needsMul) return false;
        out << "mul(";
        cg.generateExpr(expr->lhs);
        out << ", ";
        cg.generateExpr(expr->rhs);
        out << ")";
        return true;
    }

    void HLSLTarget::writeIdentifier(OmegaCommon::StrRef name, std::ostream &out) const {
        static const std::unordered_set<std::string> reserved = {
            "in", "out", "inout",
            "linear", "centroid", "sample",
            "nointerpolation", "noperspective",
            "static", "shared", "groupshared", "precise",
            "uniform", "register", "extern", "volatile",
            "column_major", "row_major",
            "string", "snorm", "unorm"
        };
        if (reserved.count(std::string(name)) > 0) {
            out << "_";
        }
        out.write(name.data(), name.size());
    }

    bool HLSLTarget::supportsPointerExpr() const { return true; }

    OmegaCommon::StrRef HLSLTarget::renameBuiltin(OmegaCommon::StrRef name) {
        /// HLSL's stdlib already uses `lerp` / `frac` / `atan2` natively,
        /// so the math builtins pass through. The `MAKE_*` constructors
        /// translate to HLSL's vector / matrix type names which double
        /// as constructors.
        ///
        /// `fma` lowers to `mad` on HLSL: HLSL's `fma` is double-only on
        /// SM 5+, while `mad` is fp32-broad. The precision contract is
        /// looser than IEEE 754 fma, but matches what every existing HLSL
        /// shader uses for "multiply-add".
        if (name == BUILTIN_FMA) return "mad";
        /// §6.2 — compute barriers. `threadgroupBarrier` is the execution +
        /// group-memory sync; `deviceBarrier` is the device-memory-only
        /// barrier (no group sync, per the portable contract).
        if (name == BUILTIN_THREADGROUP_BARRIER) return "GroupMemoryBarrierWithGroupSync";
        if (name == BUILTIN_DEVICE_BARRIER)      return "DeviceMemoryBarrier";
        if (name == BUILTIN_MAKE_FLOAT2)   return "float2";
        if (name == BUILTIN_MAKE_FLOAT3)   return "float3";
        if (name == BUILTIN_MAKE_FLOAT4)   return "float4";
        if (name == BUILTIN_MAKE_INT2)     return "int2";
        if (name == BUILTIN_MAKE_INT3)     return "int3";
        if (name == BUILTIN_MAKE_INT4)     return "int4";
        if (name == BUILTIN_MAKE_UINT2)    return "uint2";
        if (name == BUILTIN_MAKE_UINT3)    return "uint3";
        if (name == BUILTIN_MAKE_UINT4)    return "uint4";
        /// §4.1 16-bit families. The HLSL spellings only exist with
        /// `-enable-16bit-types`; the runtime feature gate
        /// (`OMEGASL_FEATURE_BIT_FLOAT16`) is what protects callers
        /// from compiling these on devices that can't run them.
        /// HLSL has no shorthand `int16_tN` constructor — use the
        /// generic `vector<T,N>` form so SM 6.2 accepts the call.
        if (name == BUILTIN_MAKE_HALF2)    return "vector<float16_t,2>";
        if (name == BUILTIN_MAKE_HALF3)    return "vector<float16_t,3>";
        if (name == BUILTIN_MAKE_HALF4)    return "vector<float16_t,4>";
        if (name == BUILTIN_MAKE_SHORT2)   return "vector<int16_t,2>";
        if (name == BUILTIN_MAKE_SHORT3)   return "vector<int16_t,3>";
        if (name == BUILTIN_MAKE_SHORT4)   return "vector<int16_t,4>";
        if (name == BUILTIN_MAKE_USHORT2)  return "vector<uint16_t,2>";
        if (name == BUILTIN_MAKE_USHORT3)  return "vector<uint16_t,3>";
        if (name == BUILTIN_MAKE_USHORT4)  return "vector<uint16_t,4>";
        /// §4.2 64-bit ints — SM 6.0+.
        if (name == BUILTIN_MAKE_LONG2)    return "vector<int64_t,2>";
        if (name == BUILTIN_MAKE_LONG3)    return "vector<int64_t,3>";
        if (name == BUILTIN_MAKE_LONG4)    return "vector<int64_t,4>";
        if (name == BUILTIN_MAKE_ULONG2)   return "vector<uint64_t,2>";
        if (name == BUILTIN_MAKE_ULONG3)   return "vector<uint64_t,3>";
        if (name == BUILTIN_MAKE_ULONG4)   return "vector<uint64_t,4>";
        /// OmegaSL `floatCxR` (C cols × R rows, host's `Matrix<float,C,R>`)
        /// emits as HLSL `floatRxC` so HLSL's row-first source-level
        /// indexing aligns with OmegaSL's column-first convention after the
        /// `INDEX_EXPR` swap in `emitIndexExpr`. Squares are unchanged.
        /// See OmegaSL-Feature-Gap-Survey §12.1.
        if (name == BUILTIN_MAKE_FLOAT2X2) return "float2x2";
        if (name == BUILTIN_MAKE_FLOAT3X3) return "float3x3";
        if (name == BUILTIN_MAKE_FLOAT4X4) return "float4x4";
        if (name == BUILTIN_MAKE_FLOAT2X3) return "float3x2";
        if (name == BUILTIN_MAKE_FLOAT2X4) return "float4x2";
        if (name == BUILTIN_MAKE_FLOAT3X2) return "float2x3";
        if (name == BUILTIN_MAKE_FLOAT3X4) return "float4x3";
        if (name == BUILTIN_MAKE_FLOAT4X2) return "float2x4";
        if (name == BUILTIN_MAKE_FLOAT4X3) return "float3x4";
        return name;
    }

    /// HLSL `RWTexture<N>D<T>::operator[]` indexes by `uint`/`uint2`/`uint3`.
    /// OmegaSL allows signed coord arithmetic (e.g. `int2(x, y)`), and HLSL
    /// 5.1 accepts the implicit conversion, but stricter SM 6.x DXC settings
    /// warn. Wrap the coord in the matching unsigned type at emit time.
    /// `uint2(uint2_v)` is a no-op, so casting unconditionally based on the
    /// texture type is safe. Returns nullptr when the texture type cannot
    /// be resolved (caller emits the coord unmodified).
    static const char *hlslUintCoordTypeForTexture(CodeGen &cg, ast::Expr *texArg){
        using namespace ast;
        TypeExpr *texTypeExpr = texArg->resolvedType;
        if(!texTypeExpr && texArg->type == ID_EXPR){
            auto *resourceId = static_cast<IdExpr *>(texArg);
            auto it = cg.resourceStore.find(resourceId->id);
            if(it != cg.resourceStore.end()){
                texTypeExpr = (*it)->typeExpr;
            }
        }
        if(!texTypeExpr) return nullptr;
        auto *texTy = cg.typeResolver->resolveTypeWithExpr(texTypeExpr);
        if(texTy == builtins::texture1d_type) return "uint";
        if(texTy == builtins::texture2d_type) return "uint2";
        if(texTy == builtins::texture3d_type) return "uint3";
        if(texTy == builtins::texture1d_array_type) return "uint2";
        if(texTy == builtins::texture2d_array_type) return "uint3";
        return nullptr;
    }

    const char *HLSLTarget::shaderObjectFileExt(ast::ShaderDecl::Type /*stage*/) const {
        return ".cso";
    }

    void HLSLTarget::emitStructDecl(CodeGen &cg, ast::StructDecl *_decl) {
        std::ostringstream out;
        out << "struct " << _decl->name << "{" << std::endl;
        /// HLSL semantics must be unique within a struct, so successive bare
        /// `Color` / `TexCoord` varyings auto-index per struct: COLOR0,
        /// COLOR1, ... and TEXCOORD0, TEXCOORD1, ... This matches MSL/GLSL,
        /// which both already permit multiple. `Color(N)` is fragment-output
        /// (SV_TargetN) and is unaffected. The struct text is cached in
        /// `generatedStructs` and reused on both producer and consumer
        /// sides, so vertex-out and fragment-in see identical indices.
        unsigned bareColorIdx = 0;
        unsigned bareTexCoordIdx = 0;
        for (auto &f : _decl->fields) {
            out << "    " << std::flush;
            cg.writeTypeExpr(f.typeExpr, out);
            out << " " << f.name;
            if (f.attributeName.has_value()) {
                out << ":";
                if (f.attributeName.value() == ATTRIBUTE_COLOR
                    && !f.attributeIndex.has_value()) {
                    out << "COLOR" << bareColorIdx++;
                } else if (f.attributeName.value() == ATTRIBUTE_TEXCOORD
                           && !f.attributeIndex.has_value()) {
                    out << "TEXCOORD" << bareTexCoordIdx++;
                } else {
                    writeAttribute(f.attributeName.value(), f.attributeIndex, out);
                }
            }
            out << ";" << std::endl;
        }
        out << "};" << std::endl;
        generatedStructs.insert(std::make_pair(_decl->name, out.str()));
    }

    void HLSLTarget::emitShaderUsedStructs(CodeGen &cg, ast::ShaderDecl *_decl,
                                           std::ostream &out) {
        OmegaCommon::Vector<OmegaCommon::String> struct_names;
        cg.typeResolver->getStructsInFuncDecl(_decl, struct_names);
        for (auto &s : struct_names) {
            out << generatedStructs[s] << std::endl;
        }
    }

    /// §6.1 — HLSL `groupshared` must be declared at global scope, so each
    /// top-level `threadgroup` local in the compute body is hoisted here as
    /// `groupshared T name[dims];`. The body walk skips the original decl.
    void HLSLTarget::emitThreadgroupGlobals(CodeGen &cg, ast::ShaderDecl *_decl,
                                            std::ostream &out) {
        if (_decl->shaderType != ast::ShaderDecl::Compute || !_decl->block) {
            return;
        }
        for (auto *stmt : _decl->block->body) {
            if (stmt->type != VAR_DECL) continue;
            auto *_var = (ast::VarDecl *)stmt;
            if (!_var->isThreadgroup) continue;
            out << "groupshared ";
            cg.writeTypeExpr(_var->typeExpr, out);
            out << " ";
            writeIdentifier(_var->spec.name, out);
            for (unsigned dim : _var->typeExpr->arrayDims) {
                out << "[" << dim << "]";
            }
            out << ";" << std::endl;
        }
    }

    void HLSLTarget::emitShaderEntryHeader(CodeGen &cg,
                                           ast::ShaderDecl *_decl,
                                           omegasl_shader &shaderDesc,
                                           std::ostream &out) {
        /// Set shader-map type/name + emit resources at file scope.
        shaderDesc.type = _decl->shaderType == ast::ShaderDecl::Vertex     ? OMEGASL_SHADER_VERTEX
                          : _decl->shaderType == ast::ShaderDecl::Fragment ? OMEGASL_SHADER_FRAGMENT
                          : _decl->shaderType == ast::ShaderDecl::Compute  ? OMEGASL_SHADER_COMPUTE
                          : _decl->shaderType == ast::ShaderDecl::Hull     ? OMEGASL_SHADER_HULL
                                                                           : OMEGASL_SHADER_DOMAIN;
        shaderDesc.name = new char[_decl->name.size() + 1];
        std::copy(_decl->name.begin(), _decl->name.end(), (char *)shaderDesc.name);
        ((char *)shaderDesc.name)[_decl->name.size()] = '\0';

        /// Resources land at file scope ahead of the function header.
        cg.emitResourcesAndFillLayout(_decl, shaderDesc, out);

        /// 3. Stage decorators.
        if (_decl->shaderType == ast::ShaderDecl::Compute) {
            shaderDesc.threadgroupDesc.x = _decl->threadgroupDesc.x;
            shaderDesc.threadgroupDesc.y = _decl->threadgroupDesc.y;
            shaderDesc.threadgroupDesc.z = _decl->threadgroupDesc.z;
            out << "[numthreads(" << _decl->threadgroupDesc.x << "," << _decl->threadgroupDesc.y << ","
                << _decl->threadgroupDesc.z << ")]" << std::endl;
        } else if (_decl->shaderType == ast::ShaderDecl::Hull) {
            auto &td = _decl->tessDesc;
            out << "[domain(\""
                << (td.domain == ast::ShaderDecl::TessellationDesc::Triangle ? "tri" : "quad") << "\")]"
                << std::endl;
            const char *partStr = td.partitioning == ast::ShaderDecl::TessellationDesc::Integer ? "integer"
                                  : td.partitioning == ast::ShaderDecl::TessellationDesc::FractionalEven
                                      ? "fractional_even"
                                      : "fractional_odd";
            out << "[partitioning(\"" << partStr << "\")]" << std::endl;
            const char *topoStr =
                td.outputTopology == ast::ShaderDecl::TessellationDesc::TriangleCW    ? "triangle_cw"
                : td.outputTopology == ast::ShaderDecl::TessellationDesc::TriangleCCW ? "triangle_ccw"
                                                                                      : "line";
            out << "[outputtopology(\"" << topoStr << "\")]" << std::endl;
            out << "[outputcontrolpoints(" << td.outputControlPoints << ")]" << std::endl;
        } else if (_decl->shaderType == ast::ShaderDecl::Domain) {
            auto &td = _decl->tessDesc;
            out << "[domain(\""
                << (td.domain == ast::ShaderDecl::TessellationDesc::Triangle ? "tri" : "quad") << "\")]"
                << std::endl;
        }

        /// Function signature: <return> <name>(<params with attributes>)
        writeTypeName(cg.typeResolver->resolveTypeWithExpr(_decl->returnType),
                      _decl->returnType->pointer, out);
        out << " " << _decl->name;
        out << "(";
        for (auto p_it = _decl->params.begin(); p_it != _decl->params.end(); p_it++) {
            if (p_it != _decl->params.begin()) {
                out << ",";
            }

            writeTypeName(cg.typeResolver->resolveTypeWithExpr(p_it->typeExpr),
                          p_it->typeExpr->pointer, out);
            out << " ";
            writeIdentifier(p_it->name, out);
            if (p_it->attributeName.has_value()) {
                if (p_it->attributeName.value() == ATTRIBUTE_VERTEX_ID) {
                    shaderDesc.vertexShaderInputDesc.useVertexID = true;
                } else if (p_it->attributeName.value() == ATTRIBUTE_GLOBALTHREAD_ID) {
                    shaderDesc.computeShaderParamsDesc.useGlobalThreadID = true;
                } else if (p_it->attributeName.value() == ATTRIBUTE_LOCALTHREAD_ID) {
                    shaderDesc.computeShaderParamsDesc.useLocalThreadID = true;
                } else if (p_it->attributeName.value() == ATTRIBUTE_THREADGROUP_ID) {
                    shaderDesc.computeShaderParamsDesc.useThreadGroupID = true;
                }
                out << ":";
                writeAttribute(p_it->attributeName.value(), p_it->attributeIndex, out);
            }
        }
        out << ")";
        if (_decl->shaderType == ast::ShaderDecl::Fragment) {
            /// Bare-`float4` fragment returns get `SV_TARGET` here.
            /// When the fragment returns a struct, per-field semantics
            /// (`SV_TargetN`, `SV_Depth`, ...) carry the bindings, and
            /// no trailing semantic on the function is needed.
            auto retTy = cg.typeResolver->resolveTypeWithExpr(_decl->returnType);
            if (retTy == ast::builtins::float4_type) {
                out << " : SV_TARGET";
            }
        }
    }

    void HLSLTarget::emitTextureSample(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// Texture has instance method
        cg.generateExpr(_expr->args[1]);
        out << ".Sample(";
        cg.generateExpr(_expr->args[0]);
        out << ",";
        cg.generateExpr(_expr->args[2]);
        out << ")";
    }

    void HLSLTarget::emitTextureSampleLOD(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// `tex.SampleLevel(s, c, lod)` — explicit mip level. The 4th OmegaSL
        /// arg is the LOD; HLSL takes it after the coord.
        cg.generateExpr(_expr->args[1]);
        out << ".SampleLevel(";
        cg.generateExpr(_expr->args[0]);
        out << ",";
        cg.generateExpr(_expr->args[2]);
        out << ",";
        cg.generateExpr(_expr->args[3]);
        out << ")";
    }

    void HLSLTarget::emitTextureSampleBias(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// `tex.SampleBias(s, c, bias)` — fragment-stage only on D3D
        /// because the underlying LOD selection still uses derivatives.
        cg.generateExpr(_expr->args[1]);
        out << ".SampleBias(";
        cg.generateExpr(_expr->args[0]);
        out << ",";
        cg.generateExpr(_expr->args[2]);
        out << ",";
        cg.generateExpr(_expr->args[3]);
        out << ")";
    }

    void HLSLTarget::emitTextureSampleGrad(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// `tex.SampleGrad(s, c, ddx, ddy)`. ddx/ddy rank already validated
        /// by Sema to match the texture's spatial domain.
        cg.generateExpr(_expr->args[1]);
        out << ".SampleGrad(";
        cg.generateExpr(_expr->args[0]);
        out << ",";
        cg.generateExpr(_expr->args[2]);
        out << ",";
        cg.generateExpr(_expr->args[3]);
        out << ",";
        cg.generateExpr(_expr->args[4]);
        out << ")";
    }

    void HLSLTarget::emitTextureGather(CodeGen &cg, ast::CallExpr *_expr, int channel, std::ostream &out) {
        /// `tex.Gather(s, c)` for the default form, `tex.GatherRed/Green/
        /// Blue/Alpha(s, c)` for the per-channel forms. D3D11.1+ exposes all
        /// four channel selectors directly. Sema restricts the texture shape
        /// to 2D / 2D-array / cube / cube-array.
        const char *suffix;
        switch(channel){
            case 0: suffix = "GatherRed"; break;
            case 1: suffix = "GatherGreen"; break;
            case 2: suffix = "GatherBlue"; break;
            case 3: suffix = "GatherAlpha"; break;
            default: suffix = "Gather"; break;
        }
        cg.generateExpr(_expr->args[1]);
        out << "." << suffix << "(";
        cg.generateExpr(_expr->args[0]);
        out << ",";
        cg.generateExpr(_expr->args[2]);
        out << ")";
    }

    void HLSLTarget::emitTextureRead(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        ast::TypeExpr *textureTypeExpr = _expr->args[0]->resolvedType;
        if (textureTypeExpr == nullptr && _expr->args[0]->type == ID_EXPR) {
            auto *resourceId = static_cast<ast::IdExpr *>(_expr->args[0]);
            auto resourceIt = cg.resourceStore.find(resourceId->id);
            if (resourceIt != cg.resourceStore.end()) {
                textureTypeExpr = (*resourceIt)->typeExpr;
            }
        }
        auto *textureTy =
            textureTypeExpr != nullptr ? cg.typeResolver->resolveTypeWithExpr(textureTypeExpr) : nullptr;
        cg.generateExpr(_expr->args[0]);
        out << ".Load(";
        if (textureTy == ast::builtins::texture1d_type) {
            /// HLSL `Texture1D.Load(int2(u, mip))` — mip slot.
            out << "int2(";
            cg.generateExpr(_expr->args[1]);
            out << ",0)";
        } else if (textureTy == ast::builtins::texture1d_array_type) {
            /// HLSL `Texture1DArray.Load(int3(u, layer, mip))`.
            out << "int3(";
            cg.generateExpr(_expr->args[1]);
            out << ",0)";
        } else if (textureTy == ast::builtins::texture2d_type) {
            /// HLSL `Texture2D.Load(int3(uv, mip))`.
            out << "int3(";
            cg.generateExpr(_expr->args[1]);
            out << ",0)";
        } else if (textureTy == ast::builtins::texture2d_array_type) {
            /// HLSL `Texture2DArray.Load(int4(uv, layer, mip))`.
            out << "int4(";
            cg.generateExpr(_expr->args[1]);
            out << ",0)";
        } else if (textureTy == ast::builtins::texture3d_type) {
            /// HLSL `Texture3D.Load(int4(uvw, mip))`.
            out << "int4(";
            cg.generateExpr(_expr->args[1]);
            out << ",0)";
        } else if (textureTy == ast::builtins::texture2d_ms_type
                   || textureTy == ast::builtins::texture2d_ms_array_type) {
            /// HLSL `Texture2DMS.Load(coord, sampleIndex)` and
            /// `Texture2DMSArray.Load(int3(uv, layer), sampleIndex)`.
            /// MS Load takes no mip slot. Sample index is a separate
            /// trailing argument.
            cg.generateExpr(_expr->args[1]);
            out << ",";
            cg.generateExpr(_expr->args[2]);
        } else {
            cg.generateExpr(_expr->args[1]);
        }
        out << ")";
    }

    void HLSLTarget::emitTextureWrite(CodeGen &cg, ast::CallExpr *_expr, std::ostream &out) {
        /// RWTexture<N>D write: texture[coord] = data, with coord cast to
        /// the matching unsigned type (see hlslUintCoordTypeForTexture).
        const char *coordCast = hlslUintCoordTypeForTexture(cg, _expr->args[0]);
        cg.generateExpr(_expr->args[0]);
        out << "[";
        if(coordCast){
            out << coordCast << "(";
            cg.generateExpr(_expr->args[1]);
            out << ")";
        } else {
            cg.generateExpr(_expr->args[1]);
        }
        out << "] = ";
        cg.generateExpr(_expr->args[2]);
    }

    void HLSLTarget::writeAttribute(OmegaCommon::StrRef attributeName,
                                    std::optional<unsigned> attributeIndex,
                                    std::ostream &out) {
        if (attributeName == ATTRIBUTE_VERTEX_ID) {
            out << "SV_VertexID";
        } else if (attributeName == ATTRIBUTE_POSITION) {
            out << "SV_Position";
        } else if (attributeName == ATTRIBUTE_COLOR) {
            /// Indexed `Color(N)` is a fragment-output semantic and maps to
            /// `SV_TargetN`. Bare `Color` falls through to `COLOR0` here for
            /// the rare entry-parameter case; struct fields are auto-indexed
            /// per struct in `emitStructDecl`.
            if (attributeIndex.has_value()) {
                out << "SV_Target" << attributeIndex.value();
            } else {
                out << "COLOR0";
            }
        } else if (attributeName == ATTRIBUTE_TEXCOORD) {
            /// Bare `TexCoord` on an entry parameter; struct fields are
            /// auto-indexed in `emitStructDecl`.
            out << "TEXCOORD0";
        } else if (attributeName == ATTRIBUTE_DEPTH) {
            out << "SV_Depth";
        } else if (attributeName == ATTRIBUTE_FRONTFACING) {
            out << "SV_IsFrontFace";
        } else if (attributeName == ATTRIBUTE_SAMPLEINDEX) {
            out << "SV_SampleIndex";
        } else if (attributeName == ATTRIBUTE_INPUT_COVERAGE
                   || attributeName == ATTRIBUTE_OUTPUT_COVERAGE) {
            /// HLSL uses `SV_Coverage` for both input (fragment param) and
            /// output (return-struct field) directions; the position of the
            /// declaration disambiguates.
            out << "SV_Coverage";
        } else if (attributeName == ATTRIBUTE_GLOBALTHREAD_ID) {
            out << "SV_DispatchThreadID";
        } else if (attributeName == ATTRIBUTE_LOCALTHREAD_ID) {
            out << "SV_GroupThreadID";
        } else if (attributeName == ATTRIBUTE_THREADGROUP_ID) {
            out << "SV_GroupID";
        }
    }

    void HLSLTarget::writeTypeName(ast::Type *_ty, bool pointer, std::ostream &out) {
        if (_ty == ast::builtins::bool_type) {
            out << "bool";
        } else if (_ty == ast::builtins::float_type) {
            out << "float";
        } else if (_ty == ast::builtins::float2_type) {
            out << "float2";
        } else if (_ty == ast::builtins::float3_type) {
            out << "float3";
        } else if (_ty == ast::builtins::float4_type) {
            out << "float4";
        } else if (_ty == ast::builtins::float2x2_type) {
            out << "float2x2";
        } else if (_ty == ast::builtins::float3x3_type) {
            out << "float3x3";
        } else if (_ty == ast::builtins::float4x4_type) {
            out << "float4x4";
        /// Non-square matrices are spelled with HLSL's row-first convention:
        /// OmegaSL `floatCxR` (C cols × R rows) → HLSL `floatRxC` (R rows ×
        /// C cols). Pairs with the `INDEX_EXPR` swap in
        /// `HLSLTarget::emitIndexExpr` so the source-level access aligns
        /// across all three backends. See §12.1.
        /// Memory layout (column-major) is locked by `compileShader*`'s
        /// compile-flag pair (`D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR` and
        /// `/Zpc`) — see §12.2. The `column_major` source-qualifier
        /// (belt-and-suspenders) was deferred because emitting it from
        /// `writeTypeName` would also leak into cast expressions where
        /// HLSL forbids storage qualifiers.
        } else if (_ty == ast::builtins::float2x3_type) {
            out << "float3x2";
        } else if (_ty == ast::builtins::float2x4_type) {
            out << "float4x2";
        } else if (_ty == ast::builtins::float3x2_type) {
            out << "float2x3";
        } else if (_ty == ast::builtins::float3x4_type) {
            out << "float4x3";
        } else if (_ty == ast::builtins::float4x2_type) {
            out << "float2x4";
        } else if (_ty == ast::builtins::float4x3_type) {
            out << "float3x4";
        } else if (_ty == ast::builtins::int_type) {
            out << "int";
        } else if (_ty == ast::builtins::int2_type) {
            out << "int2";
        } else if (_ty == ast::builtins::int3_type) {
            out << "int3";
        } else if (_ty == ast::builtins::int4_type) {
            out << "int4";
        } else if (_ty == ast::builtins::uint_type) {
            out << "uint";
        } else if (_ty == ast::builtins::uint2_type) {
            out << "uint2";
        } else if (_ty == ast::builtins::uint3_type) {
            out << "uint3";
        } else if (_ty == ast::builtins::uint4_type) {
            out << "uint4";
        }
        /// §4.1 16-bit family. HLSL spells these with the explicit
        /// arithmetic-type names from SM 6.2; vectors require the
        /// `vector<T,N>` template since `float16_t2` etc. aren't built
        /// into the language. The runtime gate
        /// (OMEGASL_FEATURE_BIT_FLOAT16) keeps these from running on
        /// devices that don't have `-enable-16bit-types` support.
        else if (_ty == ast::builtins::half_type)   { out << "float16_t"; }
        else if (_ty == ast::builtins::half2_type)  { out << "vector<float16_t,2>"; }
        else if (_ty == ast::builtins::half3_type)  { out << "vector<float16_t,3>"; }
        else if (_ty == ast::builtins::half4_type)  { out << "vector<float16_t,4>"; }
        else if (_ty == ast::builtins::short_type)  { out << "int16_t"; }
        else if (_ty == ast::builtins::short2_type) { out << "vector<int16_t,2>"; }
        else if (_ty == ast::builtins::short3_type) { out << "vector<int16_t,3>"; }
        else if (_ty == ast::builtins::short4_type) { out << "vector<int16_t,4>"; }
        else if (_ty == ast::builtins::ushort_type) { out << "uint16_t"; }
        else if (_ty == ast::builtins::ushort2_type){ out << "vector<uint16_t,2>"; }
        else if (_ty == ast::builtins::ushort3_type){ out << "vector<uint16_t,3>"; }
        else if (_ty == ast::builtins::ushort4_type){ out << "vector<uint16_t,4>"; }
        /// §4.2 64-bit ints. SM 6.0+; the FeatureScanner trips
        /// OMEGASL_FEATURE_BIT_INT64 when these appear.
        else if (_ty == ast::builtins::long_type)   { out << "int64_t"; }
        else if (_ty == ast::builtins::long2_type)  { out << "vector<int64_t,2>"; }
        else if (_ty == ast::builtins::long3_type)  { out << "vector<int64_t,3>"; }
        else if (_ty == ast::builtins::long4_type)  { out << "vector<int64_t,4>"; }
        else if (_ty == ast::builtins::ulong_type)  { out << "uint64_t"; }
        else if (_ty == ast::builtins::ulong2_type) { out << "vector<uint64_t,2>"; }
        else if (_ty == ast::builtins::ulong3_type) { out << "vector<uint64_t,3>"; }
        else if (_ty == ast::builtins::ulong4_type) { out << "vector<uint64_t,4>"; }
        else {
            out << _ty->name;
        }

        if (pointer) {
            out << "*";
        }
    }

    /// Predicate over OmegaSL builtin types — duplicated from Sema's
    /// private helper so we can ask "is this thing being indexed a
    /// matrix?" inside `emitIndexExpr` without leaking the Sema helper.
    static bool isOmegaSLMatrixType(ast::Type *t) {
        using namespace ast::builtins;
        return t == float2x2_type || t == float3x3_type || t == float4x4_type
            || t == float2x3_type || t == float2x4_type
            || t == float3x2_type || t == float3x4_type
            || t == float4x2_type || t == float4x3_type;
    }

    /// Row count of an OmegaSL matrix type. OmegaSL `floatCxR` has C
    /// columns of R rows; the source-level `m[col]` returns a column of
    /// R elements, so the synthesized HLSL column vector type is
    /// `floatR`.
    static unsigned omegaSLMatrixRowCount(ast::Type *t) {
        using namespace ast::builtins;
        if (t == float2x2_type || t == float3x2_type || t == float4x2_type) return 2;
        if (t == float2x3_type || t == float3x3_type || t == float4x3_type) return 3;
        if (t == float2x4_type || t == float3x4_type || t == float4x4_type) return 4;
        return 0;
    }

    static const char *omegaSLMatrixColumnVectorTypeHLSL(ast::Type *t) {
        switch (omegaSLMatrixRowCount(t)) {
            case 2: return "float2";
            case 3: return "float3";
            case 4: return "float4";
            default: return "float4";
        }
    }

    /// Rewrite OmegaSL's column-first matrix indexing into HLSL's
    /// row-first form so the same source produces the same element on
    /// every backend. Three branches:
    ///   (a) outer-of-two-level matrix index — `m[col][row]` →
    ///       HLSL `m[row][col]` (swap).
    ///   (b) single-level matrix read — `m[col]` →
    ///       `floatN(m[0][col], m[1][col], …, m[N-1][col])` where N is
    ///       the OmegaSL row count.
    ///   (c) anything else (vector subscript, buffer subscript, struct
    ///       array) — pass through unchanged.
    /// Single-level matrix writes (`m[col] = …`) are rejected by Sema,
    /// so this hook never sees that lvalue.
    /// See OmegaSL-Feature-Gap-Survey §12.1.
    void HLSLTarget::emitIndexExpr(CodeGen &cg, ast::IndexExpr *expr, std::ostream &out) {
        /// (a) Two-level matrix index: outer's lhs is itself an INDEX_EXPR
        /// whose own lhs resolves to a matrix.
        if (expr->lhs->type == INDEX_EXPR) {
            auto *inner = (ast::IndexExpr *)expr->lhs;
            ast::Type *innerLhsTy = nullptr;
            if (inner->lhs->resolvedType) {
                innerLhsTy = cg.typeResolver->resolveTypeWithExpr(inner->lhs->resolvedType);
            }
            if (innerLhsTy && isOmegaSLMatrixType(innerLhsTy)) {
                cg.generateExpr(inner->lhs);
                out << "[";
                cg.generateExpr(expr->idx_expr);
                out << "][";
                cg.generateExpr(inner->idx_expr);
                out << "]";
                return;
            }
        }

        /// (b) Single-level matrix read: synthesize the column vector.
        ast::Type *lhsTy = nullptr;
        if (expr->lhs->resolvedType) {
            lhsTy = cg.typeResolver->resolveTypeWithExpr(expr->lhs->resolvedType);
        }
        if (lhsTy && isOmegaSLMatrixType(lhsTy)) {
            unsigned rows = omegaSLMatrixRowCount(lhsTy);
            const char *colTy = omegaSLMatrixColumnVectorTypeHLSL(lhsTy);
            out << colTy << "(";
            for (unsigned i = 0; i < rows; ++i) {
                if (i > 0) out << ", ";
                cg.generateExpr(expr->lhs);
                out << "[" << i << "][";
                cg.generateExpr(expr->idx_expr);
                out << "]";
            }
            out << ")";
            return;
        }

        /// (c) Default — vector / buffer / struct-array.
        cg.generateExpr(expr->lhs);
        out << "[";
        cg.generateExpr(expr->idx_expr);
        out << "]";
    }

}
