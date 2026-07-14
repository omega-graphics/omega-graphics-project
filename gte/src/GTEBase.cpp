#include "omegaGTE/GTEBase.h"
#include "omegaGTE/GTEShader.h"
#include "../src/BufferIO.h"

#ifdef TARGET_METAL
// Use strict Metal simd data type alignment
#include <simd/simd.h>
#endif


_NAMESPACE_BEGIN_

const long double PI = std::acos(-1);

size_t omegaSLStructStride(OmegaCommon::Vector<omegasl_data_type> data,
                           BufferDescriptor::Role role) noexcept{
    /// §2.4 std140 path (GLSL `uniform` / HLSL `cbuffer`, column-major).
    /// Only Vulkan and D3D12 use std140 for uniforms; Metal reads constant
    /// buffers with its natural layout, so on Metal `role` is ignored and a
    /// uniform falls through to the Metal-native std430 body below.
#if !defined(TARGET_METAL)
    if(role == BufferDescriptor::Uniform){
        return std140StructStride(data);
    }
#else
    (void)role;
#endif

    /// §2.4-1 — storage / std430 path, align-then-place (replaces the prior
    /// `biggestWord` heuristic, which mis-sized any field order that
    /// interleaved sub-16 members with larger ones — e.g. `{float, float4}`
    /// allocated 24 bytes while the writer now lays the `float4` at offset 16,
    /// needing 32, a buffer overflow). The allocation must equal what the
    /// matching GE*BufferWriter packs.
    ///
    /// Vulkan / D3D12 use the *logical* std430 layout (vec3 = 12), shared with
    /// the unit-tested header helper. Metal keeps `simd_*`-native sizes (vec3 =
    /// 16, the size of `simd_float3`) so the bytes match what MSL `device T*` /
    /// `constant T&` reads; only the per-member size differs, the alignment
    /// rule is identical.
#if defined(TARGET_DIRECTX)
    /// D3D12 storage buffers are native `StructuredBuffer<T>`s — scalar (DX)
    /// layout, matching the GED3D12BufferWriter/Reader and the shader's
    /// StructuredBuffer element stride. std430's vec3→16 per-column matrix pad
    /// does not exist in a StructuredBuffer, so sizing/stride must use the
    /// tighter DX layout (e.g. float3x3 = 36, not 48) or multi-element indexing
    /// and the host↔GPU member offsets disagree.
    return structStride(data, BufferLayoutStd::DXStructured);
#elif !defined(TARGET_METAL)
    return std430StructStride(data);
#else
    size_t off = 0, structAlign = 1;
    for(auto d : data){
        size_t align = memberBaseAlignment(d, BufferLayoutStd::Std430);
        size_t size;
        if(isMatrixDataType(d)){
            auto [cols, rows] = matrixDims(d);
            size = std430MatrixSize(cols, rows);
        }
        else {
            switch (d) {
                case OMEGASL_FLOAT2 : case OMEGASL_INT2 : case OMEGASL_UINT2 :
                    size = sizeof(simd_float2); break;
                case OMEGASL_FLOAT3 : case OMEGASL_INT3 : case OMEGASL_UINT3 :
                    size = sizeof(simd_float3); break; // 16, not std430's 12
                case OMEGASL_FLOAT4 : case OMEGASL_INT4 : case OMEGASL_UINT4 :
                    size = sizeof(simd_float4); break;
                default : size = sizeof(float); break; // scalar
            }
        }
        if(align > structAlign) structAlign = align;
        off = alignOffset(off, align);
        off += size;
    }
    return alignOffset(off, structAlign);
#endif
}

OmegaCommon::Vector<size_t> omegaSLStructMemberOffsets(OmegaCommon::Vector<omegasl_data_type> data,
                                                       BufferDescriptor::Role role) noexcept {
    /// Mirrors `omegaSLStructStride` branch for branch — same standard per
    /// backend, same align-then-place walk. It has to: the two are read
    /// together (stride to size the allocation, offsets to place the members
    /// inside it), and any drift between them is a silent buffer-layout bug of
    /// exactly the kind this function exists to prevent.
    OmegaCommon::Vector<size_t> offsets;
    offsets.reserve(data.size());

#if !defined(TARGET_METAL)
    const BufferLayoutStd layout =
        (role == BufferDescriptor::Uniform) ? BufferLayoutStd::Std140 :
    #if defined(TARGET_DIRECTX)
            BufferLayoutStd::DXStructured;
    #else
            BufferLayoutStd::Std430;
    #endif
    size_t off = 0;
    for(auto d : data){
        const size_t align = memberBaseAlignment(d, layout);
        size_t size;
        if(isMatrixDataType(d)){
            auto [cols, rows] = matrixDims(d);
            size = matrixSize(cols, rows, layout);
        }
        else {
            size = std140ScalarVec(d).second; // scalar/vec size is standard-independent
        }
        off = alignOffset(off, align);
        offsets.push_back(off);
        off += size;
    }
#else
    (void)role;
    size_t off = 0;
    for(auto d : data){
        const size_t align = memberBaseAlignment(d, BufferLayoutStd::Std430);
        size_t size;
        if(isMatrixDataType(d)){
            auto [cols, rows] = matrixDims(d);
            size = std430MatrixSize(cols, rows);
        }
        else {
            switch (d) {
                case OMEGASL_FLOAT2 : case OMEGASL_INT2 : case OMEGASL_UINT2 :
                    size = sizeof(simd_float2); break;
                case OMEGASL_FLOAT3 : case OMEGASL_INT3 : case OMEGASL_UINT3 :
                    size = sizeof(simd_float3); break; // 16, not std430's 12
                case OMEGASL_FLOAT4 : case OMEGASL_INT4 : case OMEGASL_UINT4 :
                    size = sizeof(simd_float4); break;
                default : size = sizeof(float); break; // scalar
            }
        }
        off = alignOffset(off, align);
        offsets.push_back(off);
        off += size;
    }
#endif
    return offsets;
}

PixelFormatInfo pixelFormatInfo(PixelFormat fmt) {
    using A = PixelFormatInfo::Aspect;

    /// Uncompressed: {aspect, bytesPerTexel, channels, srgb}.
    auto plain = [](A aspect, std::uint8_t bpt, std::uint8_t channels, bool srgb = false){
        PixelFormatInfo i;
        i.aspect        = aspect;
        i.bytesPerTexel = bpt;
        i.channelCount  = channels;
        i.isSRGB        = srgb;
        return i;
    };
    /// Compressed: bytesPerTexel is 0 — size is derived from the block, so a
    /// caller that multiplies width*height*bytesPerTexel gets 0 rather than a
    /// plausible-but-wrong byte count.
    auto block = [](std::uint8_t bw, std::uint8_t bh, std::uint8_t bytes,
                    std::uint8_t channels, bool srgb = false){
        PixelFormatInfo i;
        i.aspect        = A::Color;
        i.bytesPerTexel = 0;
        i.blockWidth    = bw;
        i.blockHeight   = bh;
        i.blockBytes    = bytes;
        i.isCompressed  = true;
        i.channelCount  = channels;
        i.isSRGB        = srgb;
        return i;
    };

    switch (fmt) {
        // ── 8-bit color ──
        case PixelFormat::R8Unorm:            return plain(A::Color, 1, 1);
        case PixelFormat::R8Snorm:            return plain(A::Color, 1, 1);
        case PixelFormat::R8Uint:             return plain(A::Color, 1, 1);
        case PixelFormat::RG8Unorm:           return plain(A::Color, 2, 2);
        case PixelFormat::RG8Snorm:           return plain(A::Color, 2, 2);
        case PixelFormat::RGBA8Unorm:         return plain(A::Color, 4, 4);
        case PixelFormat::RGBA8Unorm_SRGB:    return plain(A::Color, 4, 4, true);
        case PixelFormat::RGBA8Snorm:         return plain(A::Color, 4, 4);
        case PixelFormat::BGRA8Unorm:         return plain(A::Color, 4, 4);
        case PixelFormat::BGRA8Unorm_SRGB:    return plain(A::Color, 4, 4, true);

        // ── 16-bit color ──
        case PixelFormat::R16Unorm:           return plain(A::Color, 2, 1);
        case PixelFormat::R16Float:           return plain(A::Color, 2, 1);
        case PixelFormat::R16Uint:            return plain(A::Color, 2, 1);
        case PixelFormat::RG16Unorm:          return plain(A::Color, 4, 2);
        case PixelFormat::RG16Float:          return plain(A::Color, 4, 2);
        case PixelFormat::RGBA16Unorm:        return plain(A::Color, 8, 4);
        case PixelFormat::RGBA16Float:        return plain(A::Color, 8, 4);

        // ── 32-bit color ──
        case PixelFormat::R32Float:           return plain(A::Color, 4, 1);
        case PixelFormat::R32Uint:            return plain(A::Color, 4, 1);
        case PixelFormat::RG32Float:          return plain(A::Color, 8, 2);
        case PixelFormat::RGBA32Float:        return plain(A::Color, 16, 4);

        // ── Packed ──
        case PixelFormat::RGB10A2Unorm:       return plain(A::Color, 4, 4);
        case PixelFormat::R11G11B10Float:     return plain(A::Color, 4, 3);

        // ── Depth / stencil ──
        case PixelFormat::D16Unorm:           return plain(A::Depth, 2, 1);
        case PixelFormat::D32Float:           return plain(A::Depth, 4, 1);
        case PixelFormat::D24Unorm_S8Uint:    return plain(A::DepthStencil, 4, 2);
        /// 5 logical bytes (32-bit depth + 8-bit stencil); every backend pads
        /// this to 8 in memory.
        case PixelFormat::D32Float_S8Uint:    return plain(A::DepthStencil, 8, 2);

        // ── BC (8-byte blocks for BC1; 16-byte for BC3/BC5/BC7) ──
        case PixelFormat::BC1_RGBA_Unorm:     return block(4, 4, 8, 4);
        case PixelFormat::BC1_RGBA_Unorm_SRGB:return block(4, 4, 8, 4, true);
        case PixelFormat::BC3_RGBA_Unorm:     return block(4, 4, 16, 4);
        case PixelFormat::BC3_RGBA_Unorm_SRGB:return block(4, 4, 16, 4, true);
        case PixelFormat::BC5_RG_Unorm:       return block(4, 4, 16, 2);
        case PixelFormat::BC7_RGBA_Unorm:     return block(4, 4, 16, 4);
        case PixelFormat::BC7_RGBA_Unorm_SRGB:return block(4, 4, 16, 4, true);

        // ── ASTC (every block is 16 bytes; only the footprint changes) ──
        case PixelFormat::ASTC_4x4_Unorm:     return block(4, 4, 16, 4);
        case PixelFormat::ASTC_4x4_Unorm_SRGB:return block(4, 4, 16, 4, true);
        case PixelFormat::ASTC_6x6_Unorm:     return block(6, 6, 16, 4);
        case PixelFormat::ASTC_6x6_Unorm_SRGB:return block(6, 6, 16, 4, true);
        case PixelFormat::ASTC_8x8_Unorm:     return block(8, 8, 16, 4);
        case PixelFormat::ASTC_8x8_Unorm_SRGB:return block(8, 8, 16, 4, true);

        // ── ETC2 / EAC ──
        case PixelFormat::ETC2_RGB8_Unorm:      return block(4, 4, 8, 3);
        case PixelFormat::ETC2_RGB8_Unorm_SRGB: return block(4, 4, 8, 3, true);
        case PixelFormat::ETC2_RGBA8_Unorm:     return block(4, 4, 16, 4);
        case PixelFormat::ETC2_RGBA8_Unorm_SRGB:return block(4, 4, 16, 4, true);
        case PixelFormat::EAC_R11_Unorm:        return block(4, 4, 8, 1);
    }
    /// Unrecognized value — RGBA8Unorm-shaped default (total function).
    return plain(A::Color, 4, 4);
}

_NAMESPACE_END_