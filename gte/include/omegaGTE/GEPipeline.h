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

    struct  OMEGAGTE_EXPORT RenderPipelineDescriptor {
        OmegaCommon::String name;
        SharedHandle<GTEShader> vertexFunc;
        SharedHandle<GTEShader> fragmentFunc;
        PixelFormat colorPixelFormat = PixelFormat::RGBA8Unorm;
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
    using GERenderPipelineState = struct __GERenderPipelineState;
    using GEComputePipelineState = struct __GEComputePipelineState;


_NAMESPACE_END_

#endif
