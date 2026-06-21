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

_NAMESPACE_END_