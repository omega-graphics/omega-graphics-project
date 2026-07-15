#include "GTEBase.h"
#include "GTEShader.h"
#include <cstdint>

#ifndef OMEGAGTE_GEPIPELINE_H
#define OMEGAGTE_GEPIPELINE_H

#ifdef None
#undef None
#endif

#if defined(TARGET_METAL) && defined(__OBJC__)
@protocol MTLLibrary;
#endif

_NAMESPACE_BEGIN_

    enum class RasterCullMode : int {
        None = 0x00,
        Front,
        Back
    };

    enum class TriangleFillMode : int {
        Wireframe = 0x00,
        Solid
    };

    enum class CompareFunc : int {
        Less,
        LessEqual,
        Greater,
        GreaterEqual
    };

    enum class StencilOperation : int {
        Retain,
        Zero,
        Replace,
        IncrementWrap,
        DecrementWrap
    };

    enum class DepthWriteAmount : int {
        Zero,
        All
    };

    /// @brief Format of a vertex attribute in a vertex buffer element.
    enum class VertexFormat : uint8_t {
        Float,       // 4 bytes
        Float2,      // 8 bytes
        Float3,      // 12 bytes
        Float4,      // 16 bytes
        Int,         // 4 bytes
        Int2,        // 8 bytes
        Int3,        // 12 bytes
        Int4,        // 16 bytes
        UInt,        // 4 bytes
        UInt2,       // 8 bytes
        UInt3,       // 12 bytes
        UInt4,       // 16 bytes
        UNorm8x4,    // 4 bytes, normalized
        SNorm8x4,    // 4 bytes, normalized
        UShort2,     // 4 bytes
        UShort4,     // 8 bytes
        Half2,       // 4 bytes
        Half4        // 8 bytes
    };

    /// @brief How the vertex input rate advances: per-vertex or per-instance.
    enum class VertexStepFunction : uint8_t {
        PerVertex,
        PerInstance
    };

    /// @brief Describes one vertex buffer binding slot (stride + step rate).
    struct OMEGAGTE_EXPORT VertexBufferLayout {
        unsigned stride = 0;
        VertexStepFunction stepFunction = VertexStepFunction::PerVertex;
        unsigned stepRate = 1;  ///< 1 for per-vertex; instance divisor for per-instance.
    };

    /// @brief Describes one vertex shader input attribute.
    struct OMEGAGTE_EXPORT VertexAttribute {
        unsigned bufferIndex = 0;                        ///< Which vertex buffer slot this attribute reads from.
        unsigned offset = 0;                             ///< Byte offset within the element.
        VertexFormat format = VertexFormat::Float4;
        unsigned shaderLocation = 0;                     ///< Maps to OmegaSL input location.
    };

    /// @brief Complete vertex input layout for a render pipeline. If empty,
    /// backends fall back to the shader-reflected layout.
    struct OMEGAGTE_EXPORT VertexInputDescriptor {
        OmegaCommon::Vector<VertexBufferLayout> bufferLayouts;
        OmegaCommon::Vector<VertexAttribute> attributes;
    };

    /// @brief Blend factor options. Dual-source (Src1*) requires GTEDeviceFeatures::dualSourceBlending.
    enum class BlendFactor : uint8_t {
        Zero,
        One,
        SrcColor,
        InvSrcColor,
        SrcAlpha,
        InvSrcAlpha,
        DestColor,
        InvDestColor,
        DestAlpha,
        InvDestAlpha,
        SrcAlphaSaturated,
        Src1Color,
        InvSrc1Color,
        Src1Alpha,
        InvSrc1Alpha
    };

    /// @brief Blend equation operation.
    enum class BlendOperation : uint8_t {
        Add,
        Subtract,
        ReverseSubtract,
        Min,
        Max
    };

    /// @brief Color write mask bits. OR together.
    enum ColorWriteMask : uint8_t {
        ColorWriteNone  = 0,
        ColorWriteRed   = 1 << 0,
        ColorWriteGreen = 1 << 1,
        ColorWriteBlue  = 1 << 2,
        ColorWriteAlpha = 1 << 3,
        ColorWriteAll   = 0xF
    };

    /// @brief Per-color-attachment blend state. If no blend descriptor is
    /// supplied for an attachment, blending is disabled (opaque write).
    struct OMEGAGTE_EXPORT BlendDescriptor {
        bool blendEnabled = false;
        BlendFactor srcColorFactor = BlendFactor::SrcAlpha;
        BlendFactor destColorFactor = BlendFactor::InvSrcAlpha;
        BlendOperation colorOp = BlendOperation::Add;
        BlendFactor srcAlphaFactor = BlendFactor::One;
        BlendFactor destAlphaFactor = BlendFactor::InvSrcAlpha;
        BlendOperation alphaOp = BlendOperation::Add;
        uint8_t writeMask = ColorWriteAll;
    };

    /// @brief Primitive category a render pipeline rasterizes. Maps to
    /// D3D12's `PrimitiveTopologyType`; Metal / Vulkan pick up the actual
    /// topology at draw time and ignore this field.
    enum class PrimitiveTopologyCategory : uint8_t {
        Triangle,
        Line,
        Point,
        /// §16 Phase E — a tessellation-patch pipeline. Drives D3D12's
        /// `D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH`; Metal / Vulkan pick the
        /// control-point patch topology up at draw time from `drawPatches`.
        Patch
    };

    struct  OMEGAGTE_EXPORT RenderPipelineDescriptor {
        OmegaCommon::String name;
        SharedHandle<GTEShader> vertexFunc;
        SharedHandle<GTEShader> fragmentFunc;
        /// §16 Phase E — optional tessellation stages. When both @c hullFunc
        /// and @c domainFunc are set, this is a tessellation pipeline: the hull
        /// (an OmegaSL `hull` stage) runs as a compute pre-pass that computes
        /// per-patch tessellation factors + per-control-point output, and the
        /// domain (an OmegaSL `domain` stage) becomes the post-tessellation
        /// vertex function in place of @c vertexFunc. Leave both null for a
        /// normal vertex/fragment pipeline; @c vertexFunc is then required as
        /// before. Feature-gated behind @c GTEDEVICE_FEATURE_TESSELLATION_SHADER
        /// — @c makeRenderPipelineState returns @c nullptr with a diagnostic on
        /// a device that does not advertise it.
        SharedHandle<GTEShader> hullFunc;
        SharedHandle<GTEShader> domainFunc;
        /// Control points per input patch. Must match the hull's
        /// `outputcontrolpoints`. Only consulted when @c hullFunc /
        /// @c domainFunc are set; 0 otherwise. The runtime also reads the
        /// authoritative count from the hull's serialized
        /// `omegasl_tessellation_desc`, so this is a convenience / validation
        /// value at the API surface.
        uint32_t patchControlPoints = 0;
        /// Pixel formats of the color attachments this pipeline writes to.
        /// Index `i` corresponds to color attachment `i`. Empty vector is
        /// treated as a single default-format attachment.
        OmegaCommon::Vector<PixelFormat> colorPixelFormats = { PixelFormat::RGBA8Unorm };
        /// Format of the depth (or depth/stencil) attachment this pipeline is
        /// tested against. Must match the `depthTexture` of the render target the
        /// pipeline is used with.
        ///
        /// Consulted only when `depthAndStencilDesc.enableDepth` /
        /// `enableStencil` is set; ignored otherwise, which is why the default is
        /// a harmless color format rather than a depth one. Metal in particular
        /// requires `depthAttachmentPixelFormat` on the pipeline — without it a
        /// depth-enabled pipeline is silently inert, which is one of the reasons
        /// depth testing has never worked here.
        PixelFormat depthStencilPixelFormat = PixelFormat::D32Float;
        /// Primitive category the pipeline rasterizes. Must match the
        /// polygon type passed to draw commands bound to this pipeline on
        /// D3D12. Ignored on Metal / Vulkan.
        PrimitiveTopologyCategory primitiveTopologyCategory = PrimitiveTopologyCategory::Triangle;
        unsigned rasterSampleCount = 0;
        RasterCullMode cullMode = RasterCullMode::None;
        TriangleFillMode triangleFillMode = TriangleFillMode::Solid;
        GTEPolygonFrontFaceRotation polygonFrontFaceRotation = GTEPolygonFrontFaceRotation::Clockwise;
        /// Vertex input layout. If `attributes` is empty the backend infers the layout
        /// from the vertex shader's reflected inputs (legacy behavior).
        VertexInputDescriptor vertexInputDescriptor;
        /// Per-attachment blend state. Index `i` maps to color attachment `i`.
        /// If empty, blending is disabled for every attachment.
        OmegaCommon::Vector<BlendDescriptor> colorBlendDescriptors;
        struct DepthStencilDesc {
            bool enableDepth = false;
            bool enableStencil = false;
            DepthWriteAmount writeAmount = DepthWriteAmount::All;
            CompareFunc depthOperation = CompareFunc::Less;
            float depthBias = 0.f,slopeScale = 0.f,depthClamp = 0.f;
            unsigned stencilReadMask = 0,stencilWriteMask = 0;
            struct StencilDesc {
                StencilOperation
                        stencilFail = StencilOperation::Replace,
                        depthFail = StencilOperation::Replace,
                        pass = StencilOperation::Retain;
                CompareFunc func = CompareFunc::Less;
            } frontFaceStencil,backFaceStencil;
        } depthAndStencilDesc;
    };

    struct  OMEGAGTE_EXPORT ComputePipelineDescriptor {
        OmegaCommon::String name;
        SharedHandle<GTEShader> computeFunc;

    };

    /// @brief Describes a programmable blit pipeline (Extension 3).
    ///
    /// A blit pipeline is a thin wrapper over a render pipeline whose vertex
    /// stage is supplied by the engine (a built-in full-screen-triangle
    /// vertex shader). The caller provides only the fragment stage and the
    /// source/destination pixel formats. At draw time, `blitWithPipeline()`
    /// reads from a source texture, runs the supplied fragment shader, and
    /// writes to a destination texture covering the full destination extent
    /// (or a user-specified subregion).
    ///
    /// **Fragment shader contract** — the user-supplied @c fragmentFunc must
    /// declare its input parameter as a struct matching the engine's
    /// rasterizer output:
    /// @code
    ///   struct OmegaGTEBlitVertexData internal {
    ///       float4 pos : Position;
    ///       float2 uv  : TexCoord;
    ///   };
    /// @endcode
    /// Any name is fine — the contract is the member layout and semantics.
    /// The fragment shader is also responsible for declaring its own
    /// @c static sampler2d for sampling the source texture; OmegaSL bakes
    /// samplers into shader sources (there is no runtime sampler binding
    /// API today). The source texture is bound at fragment-shader resource
    /// slot @c 0.
    struct OMEGAGTE_EXPORT BlitPipelineDescriptor {
        OmegaCommon::String name;
        /// @brief Fragment shader that transforms the sampled source texel.
        /// Must consume an `OmegaGTEBlitVertexData`-shaped struct (see above)
        /// and write to a single color output (`fragment float4 ...`).
        SharedHandle<GTEShader> fragmentFunc;
        /// @brief Pixel format of the source texture. Used only for
        /// validation / documentation today; not consumed by pipeline
        /// creation (textures advertise their own format).
        PixelFormat srcPixelFormat = PixelFormat::RGBA8Unorm;
        /// @brief Pixel format of the destination texture. Drives the
        /// underlying render pipeline's color-attachment format.
        PixelFormat destPixelFormat = PixelFormat::RGBA8Unorm;
        /// @brief Multisample count of the source (for MSAA-resolve blits).
        /// Defaults to 1; not consumed by pipeline creation today.
        unsigned srcSampleCount = 1;
    };

    /// @brief Describes a mesh-shader render pipeline (Mesh-Shader-Plan
    /// Phase 3). A mesh pipeline replaces the vertex-input + vertex-stage
    /// half of a render pipeline with the mesh-shader pair: an optional
    /// amplification (task) stage that dispatches mesh threadgroups, a
    /// required mesh stage that emits meshlet vertices + indices, and the
    /// usual fragment stage downstream. The rasterization / blend / depth
    /// state is the same shape as @c RenderPipelineDescriptor — the only
    /// difference is the geometry side.
    ///
    /// `vertexInputDescriptor` and `primitiveTopologyCategory` from
    /// `RenderPipelineDescriptor` are intentionally absent: the mesh
    /// stage emits its own primitives and there is no vertex-buffer
    /// pull, so neither makes sense here.
    ///
    /// Feature-gated behind @c GTEDEVICE_FEATURE_MESH_SHADER —
    /// @c OmegaGraphicsEngine::makeMeshPipelineState returns @c nullptr
    /// with a diagnostic on devices that don't advertise it. Same
    /// pattern as raytracing (see @c createBoundingBoxesBuffer /
    /// @c allocateAccelerationStructure).
    struct OMEGAGTE_EXPORT MeshPipelineDescriptor {
        OmegaCommon::String name;
        /// Optional amplification stage (task shader). Null is valid —
        /// every backend lets a mesh pipeline run without an
        /// amplification stage. Phase-5 follow-up wires the payload
        /// machinery; today only its presence at the descriptor level
        /// is honored.
        SharedHandle<GTEShader> amplificationFunc;
        /// Required mesh stage. Built from an OmegaSL
        /// `mesh(max_vertices=..., max_primitives=..., topology=...)`
        /// entry; the shader's serialized @c meshDesc carries the
        /// per-meshlet maxima the backend needs to wire the PSO.
        SharedHandle<GTEShader> meshFunc;
        /// Required fragment stage — same shape as
        /// @c RenderPipelineDescriptor::fragmentFunc.
        SharedHandle<GTEShader> fragmentFunc;

        // ── Render state, mirroring RenderPipelineDescriptor ────────
        OmegaCommon::Vector<PixelFormat> colorPixelFormats = { PixelFormat::RGBA8Unorm };
        unsigned rasterSampleCount = 0;
        RasterCullMode cullMode = RasterCullMode::None;
        TriangleFillMode triangleFillMode = TriangleFillMode::Solid;
        GTEPolygonFrontFaceRotation polygonFrontFaceRotation = GTEPolygonFrontFaceRotation::Clockwise;
        OmegaCommon::Vector<BlendDescriptor> colorBlendDescriptors;
        /// Reuses @c RenderPipelineDescriptor::DepthStencilDesc so the
        /// caller writes the same struct shape for both pipeline kinds.
        RenderPipelineDescriptor::DepthStencilDesc depthAndStencilDesc;
        /// Format of the depth attachment this pipeline is tested against — same
        /// contract as @c RenderPipelineDescriptor::depthStencilPixelFormat.
        PixelFormat depthStencilPixelFormat = PixelFormat::D32Float;
    };

    using GERenderPipelineState = struct __GERenderPipelineState;
    using GEComputePipelineState = struct __GEComputePipelineState;
    using GEBlitPipelineState = struct __GEBlitPipelineState;


_NAMESPACE_END_

#endif
