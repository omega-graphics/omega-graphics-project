#include <stdlib.h>

#ifndef omegasl_h
#define  omegasl_h

/// @name Standard *.omegasllib structs
/// @{
using CString = const char *;

struct omegasl_lib_header {
    unsigned name_length;
    CString name;
    unsigned entry_count;
};

enum omegasl_shader_layout_desc_io_mode : int {
    OMEGASL_SHADER_DESC_IO_IN,
    OMEGASL_SHADER_DESC_IO_OUT,
    OMEGASL_SHADER_DESC_IO_INOUT
};

enum omegasl_shader_layout_desc_type : int {
    OMEGASL_SHADER_CONSTANT_DESC,
    OMEGASL_SHADER_BUFFER_DESC,
    OMEGASL_SHADER_TEXTURE1D_DESC,
    OMEGASL_SHADER_TEXTURE2D_DESC,
    OMEGASL_SHADER_TEXTURE3D_DESC,

    OMEGASL_SHADER_SAMPLER1D_DESC,
    OMEGASL_SHADER_SAMPLER2D_DESC,
    OMEGASL_SHADER_SAMPLER3D_DESC,

    OMEGASL_SHADER_STATIC_SAMPLER1D_DESC,
    OMEGASL_SHADER_STATIC_SAMPLER2D_DESC,
    OMEGASL_SHADER_STATIC_SAMPLER3D_DESC,

    /// Phase A — compile-path types (see OmegaSL §2.1). Runtime descriptor
    /// binding of these is Phase B (see Pipeline-Completion-Extension-Plan.md
    /// "Texture View Type Extension"). Until Phase B lands the engine cannot
    /// create a real cube/array/MS view, so a shader using one will fail at
    /// pipeline-bind time, not at compile time.
    OMEGASL_SHADER_TEXTURE1D_ARRAY_DESC,
    OMEGASL_SHADER_TEXTURE2D_ARRAY_DESC,
    OMEGASL_SHADER_TEXTURECUBE_DESC,
    OMEGASL_SHADER_TEXTURECUBE_ARRAY_DESC,
    OMEGASL_SHADER_TEXTURE2D_MS_DESC,
    OMEGASL_SHADER_TEXTURE2D_MS_ARRAY_DESC,

    OMEGASL_SHADER_SAMPLERCUBE_DESC,
    OMEGASL_SHADER_STATIC_SAMPLERCUBE_DESC,

    /// §2.4 constant / uniform buffer (HLSL `ConstantBuffer<T>` at register
    /// `b`, MSL `constant T&`, GLSL `layout(std140) uniform`). Distinct from
    /// OMEGASL_SHADER_CONSTANT_DESC (an inline single-scalar push constant)
    /// and from OMEGASL_SHADER_BUFFER_DESC (a structured/storage buffer).
    /// Appended at the tail so existing descriptor-type integer slots are
    /// preserved. Phase A (compile path) emits this in the layout; runtime
    /// binding of it (D3D12 CBV / Vulkan UNIFORM_BUFFER) is Phase B and is
    /// not yet wired — a shader using `uniform<T>` compiles but cannot be
    /// bound until that lands.
    OMEGASL_SHADER_UNIFORM_DESC,

    /// §2.2 (Pipeline-Completion) / §10.2 push constant (`constant<T>`).
    /// Small (≤128 bytes portable), read-only, value-access constant data
    /// bound *without* a descriptor/buffer allocation: D3D12 root constants
    /// (`SetGraphics/ComputeRoot32BitConstants` at register `b`), Metal
    /// `setBytes:length:atIndex:`, Vulkan `vkCmdPushConstants`
    /// (`VkPushConstantRange`). Distinct from OMEGASL_SHADER_UNIFORM_DESC (a
    /// CBV / bound uniform buffer), from OMEGASL_SHADER_CONSTANT_DESC (the
    /// inline single-scalar push constant with a baked default value), and
    /// from OMEGASL_SHADER_BUFFER_DESC (a structured/storage buffer).
    /// Appended at the tail so existing descriptor-type integer slots are
    /// preserved. Phase A (compile path) emits this in the layout; runtime
    /// binding of it (root constants / setBytes / vkCmdPushConstants + the
    /// `setRenderConstants`/`setComputeConstants` command API and the
    /// pipeline-layout push range reservation) is Phase B and is not yet
    /// wired — a shader using `constant<T>` compiles but cannot be bound
    /// until that lands.
    OMEGASL_SHADER_PUSH_CONSTANT_DESC
};

enum omegasl_shader_static_sampler_filter : int {
    OMEGASL_SHADER_SAMPLER_LINEAR_FILTER,
    OMEGASL_SHADER_SAMPLER_POINT_FILTER,
    OMEGASL_SHADER_SAMPLER_MAX_ANISOTROPY_FILTER,
    OMEGASL_SHADER_SAMPLER_MIN_ANISOTROPY_FILTER
};

enum omegasl_shader_static_sampler_address_mode : int {
    OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_CLAMPTOEDGE,
    OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_WRAP,
    OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_MIRROR,
    OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_MIRRORWRAP
};

struct omegasl_shader_static_sampler_desc {
    omegasl_shader_static_sampler_filter filter;
    omegasl_shader_static_sampler_address_mode u_address_mode,v_address_mode,w_address_mode;
    unsigned int max_anisotropy;
};

enum omegasl_data_type : int {
    //Int Types
    OMEGASL_INT,
    OMEGASL_INT2,
    OMEGASL_INT3,
    OMEGASL_INT4,

    //Uint Types
    OMEGASL_UINT,
    OMEGASL_UINT2,
    OMEGASL_UINT3,
    OMEGASL_UINT4,

    //Float Types
    OMEGASL_FLOAT,
    OMEGASL_FLOAT2,
    OMEGASL_FLOAT3,
    OMEGASL_FLOAT4,
    OMEGASL_FLOAT2x1,
    OMEGASL_FLOAT2x2,
    OMEGASL_FLOAT2x3,
    OMEGASL_FLOAT2x4,
    OMEGASL_FLOAT3x1,
    OMEGASL_FLOAT3x2,
    OMEGASL_FLOAT3x3,
    OMEGASL_FLOAT3x4,
    OMEGASL_FLOAT4x1,
    OMEGASL_FLOAT4x2,
    OMEGASL_FLOAT4x3,
    OMEGASL_FLOAT4x4,

    OMEGASL_DOUBLE,
    OMEGASL_DOUBLE2,
    OMEGASL_DOUBLE3,
    OMEGASL_DOUBLE4,

    /// §12.2 follow-up — integer matrix types for the host-side buffer R/W
    /// API (`GEBufferWriter::writeInt4x4`, etc.). Appended at the tail rather
    /// than grouped with the integer scalars/vectors above so the numeric
    /// value of every pre-existing enumerator is preserved — these values are
    /// never serialized into `.omegasllib` archives (codegen does not emit
    /// matrix data types), so only the in-memory C++ ABI matters. An int/uint
    /// matrix's std430/std140 byte layout is identical to the same-shape
    /// `OMEGASL_FLOATCxR` (int/uint/float are all 4-byte scalars).
    OMEGASL_INT2x2,
    OMEGASL_INT2x3,
    OMEGASL_INT2x4,
    OMEGASL_INT3x2,
    OMEGASL_INT3x3,
    OMEGASL_INT3x4,
    OMEGASL_INT4x2,
    OMEGASL_INT4x3,
    OMEGASL_INT4x4,
    OMEGASL_UINT2x2,
    OMEGASL_UINT2x3,
    OMEGASL_UINT2x4,
    OMEGASL_UINT3x2,
    OMEGASL_UINT3x3,
    OMEGASL_UINT3x4,
    OMEGASL_UINT4x2,
    OMEGASL_UINT4x3,
    OMEGASL_UINT4x4

};

struct omegasl_shader_constant_desc {
    omegasl_data_type type;
    union V {
        float f;
        double d;
        int i;
        unsigned int ui;
    } value;
    static omegasl_shader_constant_desc make_float(float f){
        omegasl_shader_constant_desc::V st{};
        st.f = f;
        return {OMEGASL_FLOAT,st};
    };
    static omegasl_shader_constant_desc make_double(double d){
        omegasl_shader_constant_desc::V st{};
        st.d = d;
        return {OMEGASL_DOUBLE,st};
    };
    static omegasl_shader_constant_desc make_int(int i){
        omegasl_shader_constant_desc::V st{};
        st.i = i;
        return {OMEGASL_INT,st};
    };
    static omegasl_shader_constant_desc make_uint(unsigned int ui){
        omegasl_shader_constant_desc::V st{};
        st.ui = ui;
        return {OMEGASL_UINT,st};
    };
};

/// Default channel swizzle baked into a texture resource at the layout
/// (shader-library) level. The encoding is *distinct* from the runtime
/// `TextureSwizzleChannel` enum so that a zero-initialized descriptor
/// (the default for every layout entry that is not a texture, and for
/// every texture entry compiled before this field existed) reads as
/// Identity, not as `Red`.
///
///   0 = Identity (passthrough)
///   1 = Red
///   2 = Green
///   3 = Blue
///   4 = Alpha
///   5 = Zero
///   6 = One
///
/// The runtime translates this encoding into `TextureSwizzle` at bind
/// time. A descriptor with all four bytes == 0 is treated as identity
/// and bypasses the per-bind swizzled-view cache.
struct omegasl_texture_swizzle_desc {
    unsigned char r;
    unsigned char g;
    unsigned char b;
    unsigned char a;
};

/// TODO(swizzle-binary-compat): `swizzle_desc` is appended to the end of
/// the layout descriptor. Zero-init reads as Identity per the encoding
/// above, so callers that aggregate-initialize `omegasl_shader_layout_desc{}`
/// keep working without source changes. Pre-existing `.omegasllib`
/// archives serialized before this field existed will, however,
/// deserialize with whatever bytes happened to follow the old struct in
/// the archive — bumping the on-disk layout-desc version to gate the
/// new field is deferred. See `gte/docs/texture-swizzle-proposal.md` §4.
struct omegasl_shader_layout_desc {
    omegasl_shader_layout_desc_type type;
    unsigned gpu_relative_loc;
    omegasl_shader_layout_desc_io_mode io_mode;
    size_t location;
    size_t offset;
    omegasl_shader_static_sampler_desc sampler_desc;
    omegasl_shader_constant_desc constant_desc;
    omegasl_texture_swizzle_desc swizzle_desc;
};

enum omegasl_shader_type : int {
    OMEGASL_SHADER_VERTEX,
    OMEGASL_SHADER_FRAGMENT,
    OMEGASL_SHADER_COMPUTE,
    OMEGASL_SHADER_HULL,
    OMEGASL_SHADER_DOMAIN,
    /// Mesh-shader pipeline mesh stage (SM6.5 `ms` / MSL `[[mesh]]` /
    /// `GL_EXT_mesh_shader`). Appended at the tail so the numeric value of
    /// every pre-existing stage is preserved in serialized `.omegasllib`
    /// archives. Gated at load time by OMEGASL_FEATURE_BIT_MESH_SHADERS.
    /// The amplification/task stage will append after this when it lands.
    OMEGASL_SHADER_MESH
};

/// Per-shader feature requirements. Each bit names a runtime feature that
/// the *generated* shader requires. The library writer sets these from the
/// `#requires` declarations + the codegen-time portability scan; the
/// loader masks them against the device's GTEDeviceFeatures and rejects
/// only the shaders whose required bits are not satisfied. Other shaders
/// in the same library load normally. New bits append at the tail;
/// existing bits never get reused. See OmegaSL-Feature-Gap-Survey §14.
enum omegasl_shader_feature_flags : unsigned long long {
    OMEGASL_FEATURE_BIT_NONE               = 0,
    OMEGASL_FEATURE_BIT_RAYTRACING         = 1ull << 0,
    OMEGASL_FEATURE_BIT_MESH_SHADERS       = 1ull << 1,
    OMEGASL_FEATURE_BIT_GEOMETRY_SHADERS   = 1ull << 2,
    OMEGASL_FEATURE_BIT_TESSELLATION       = 1ull << 3,
    OMEGASL_FEATURE_BIT_SUBGROUP_OPS       = 1ull << 4,
    OMEGASL_FEATURE_BIT_BINDLESS           = 1ull << 5,
    OMEGASL_FEATURE_BIT_FLOAT16            = 1ull << 6,
    OMEGASL_FEATURE_BIT_INT64              = 1ull << 7,
    OMEGASL_FEATURE_BIT_VARIABLE_RATE      = 1ull << 8,
    OMEGASL_FEATURE_BIT_SUBPASS_INPUTS     = 1ull << 9,
    OMEGASL_FEATURE_BIT_SPEC_CONSTANTS     = 1ull << 10,
    OMEGASL_FEATURE_BIT_TEXTURECUBE_RW     = 1ull << 11,
    OMEGASL_FEATURE_BIT_TEXTURE2D_MS_WRITE = 1ull << 12,
    OMEGASL_FEATURE_BIT_DOUBLE             = 1ull << 13,
    /// `sampleLOD` / `sampleBias` / `sampleGrad` on `texture1d` and
    /// `texture1d_array`. Apple GPUs do not support mipmap selection
    /// on 1D textures, so MSL has no `level()` / `bias()` overload of
    /// `texture1d::sample(...)` and no `gradient1d` function exists at
    /// all. HLSL and GLSL accept the call. Shaders that need this op
    /// must `#requires(TEXTURE1D_MIP_SAMPLE)`; on MSL the loader will
    /// emit a header-only stub and reject pipelines that bind it.
    OMEGASL_FEATURE_BIT_TEXTURE1D_MIP_SAMPLE = 1ull << 14,
    /// §1.7 — user cull distance (`CullDistance` semantic). HLSL
    /// (`SV_CullDistance`) and GLSL (`gl_CullDistance[]`) express it; Metal
    /// has no cull-distance equivalent, so MSL cannot. A shader using
    /// `CullDistance` must `#requires(CULL_DISTANCE)`; on MSL the compiler
    /// emits a header-only stub and the loader rejects pipelines that bind
    /// it. (Clip distance, by contrast, is universal and carries no bit.)
    OMEGASL_FEATURE_BIT_CULL_DISTANCE = 1ull << 15
};


struct omegasl_vertex_shader_param_desc {
    CString name;
    omegasl_data_type type;
    size_t offset;
};

struct omegasl_vertex_shader_input_desc {
    bool useVertexID = false;
    omegasl_vertex_shader_param_desc *pParams = nullptr;
    unsigned nParam = 0;
};

struct omegasl_compute_shader_params_desc {
    bool useGlobalThreadID = false,useLocalThreadID = false,useThreadGroupID = false;
};

struct omegasl_compute_shader_threadgroup_desc {
    unsigned x = 0,y = 0,z = 0;
};

/// Mesh-stage descriptor. `topology`: 0 = triangle, 1 = line, 2 = point
/// (mirrors ast::ShaderDecl::MeshDesc::Topology). The per-meshlet local
/// workgroup size is carried by `omegasl_shader::threadgroupDesc`, exactly
/// as for compute's [numthreads]. Appended at the tail of the shader struct
/// so the numeric value of pre-existing fields is preserved; only emitted /
/// read for `OMEGASL_SHADER_MESH` entries (see CodeGen.h writer / GE.cpp
/// reader).
struct omegasl_mesh_shader_desc {
    unsigned max_vertices = 0;
    unsigned max_primitives = 0;
    int topology = 0;
};


struct omegasl_shader {
    omegasl_shader_type type;
    CString name;
    omegasl_vertex_shader_input_desc vertexShaderInputDesc;
    omegasl_compute_shader_params_desc computeShaderParamsDesc;
    omegasl_compute_shader_threadgroup_desc threadgroupDesc;
    omegasl_mesh_shader_desc meshDesc;
    omegasl_shader_layout_desc *pLayout;
    unsigned nLayout;
    void *data;
    size_t dataSize;
    /// Bitfield of OMEGASL_FEATURE_BIT_* values declared via the source-
    /// level `#requires(...)` directive. The loader masks these against
    /// the device's feature bitmask and rejects only the shaders whose
    /// required bits are not satisfied. A `dataSize == 0` shader entry
    /// is a "stub" — the active backend cannot express one of the
    /// required features, so the body was deliberately omitted. The
    /// header (type, name, requiredFeatures) is still serialized so the
    /// runtime can produce a precise rejection diagnostic.
    unsigned long long requiredFeatures;
};

/// @}


/// ── On-disk `.omegasllib` container prefix (OmegaSL linker, Phase 0) ──────
/// Every compiled `.omegasllib` archive begins with this fixed 12-byte prefix
/// so a loader or link tool can (a) recognize the file as an OmegaSL library,
/// (b) refuse a format version it does not understand, and (c) refuse to mix
/// archives built for different backends. The writer is
/// `CodeGen::linkShaderObjects` (omegasl/src/CodeGen.h); the reader is
/// `OmegaGraphicsEngine::loadShaderLibraryFromInputStream` (gte/src/GE.cpp).
/// The two MUST stay byte-identical — this header is the single source of
/// truth for the magic and version so they cannot drift.
///
///   char[4]   magic           = OMEGASLLIB_MAGIC ("OSLL")
///   uint32    format_version   = OMEGASLLIB_FORMAT_VERSION
///   uint8     backend_id       (omegasl_backend_id; numeric == Target::Kind)
///   uint8[3]  reserved         (zero)
/// followed by the existing body (libname_size, libname, entry_count, …).
#define OMEGASLLIB_MAGIC "OSLL"
#define OMEGASLLIB_MAGIC_LEN 4
#define OMEGASLLIB_FORMAT_VERSION 1u

/// Backend that produced a `.omegasllib`. Numeric values match
/// `omegasl::Target::Kind` (HLSL=0, MSL=1, GLSL=2) so the writer casts
/// directly. The engine loader only ever loads its own backend's library, so
/// it reads-and-skips this field; the link tool (later phase) uses it to
/// reject a cross-backend merge loudly.
enum omegasl_backend_id {
    OMEGASL_BACKEND_ID_HLSL = 0,
    OMEGASL_BACKEND_ID_MSL  = 1,
    OMEGASL_BACKEND_ID_GLSL = 2
};


/// Build Library
#include <omega-common/fs.h>

#include "omegaGTE/GTEBase.h"

_NAMESPACE_BEGIN_
struct GTEDevice;
_NAMESPACE_END_

#if _WIN32

#ifdef OMEGAGTE__BUILD__
#define OMEGASLC_EXPORT _declspec(dllexport)
#else
#define OMEGASLC_EXPORT __declspec(dllimport)
#endif

#else 

#define OMEGASLC_EXPORT

#endif


struct omegasl_shader_lib {
    omegasl_lib_header header;
    omegasl_shader *shaders;
};

/**
 *  @brief OmegaSL Compiler Frontend. (For Runtime Compilation Use Only)
 *  @paragraph
 *  The biggest difference between this and the `omegaslc` executable
 *  is that it invokes the target platform's runtime shader compiler
 *  (D3DCompileFromFile on Windows,
    \c \# id<MTLLibrary> [(id<MTLDevice>) newlibraryFromSource: withCompletionProvider:]
    on Apple Devices,
 *  and shaderc::CompilerGlslToSpv() on Android and Linux.)
 * */
class OMEGASLC_EXPORT OmegaSLCompiler {
public:
    struct OMEGASLC_EXPORT Source {
        static std::shared_ptr<Source> fromFile(OmegaCommon::FS::Path path);
        static std::shared_ptr<Source> fromString(OmegaCommon::String & buffer);
    };
    static std::shared_ptr<OmegaSLCompiler> Create(SharedHandle<OmegaGTE::GTEDevice> & device);
    virtual std::shared_ptr<omegasl_shader_lib> compile(std::initializer_list<std::shared_ptr<Source>> sources) = 0;
};

#endif