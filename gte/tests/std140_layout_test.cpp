/// OmegaSL §2.4 — backend-independent unit test for the std430 / std140
/// buffer memory-layout math (BufferIO.h). Pure CPU: exercises the helpers
/// directly so the std140 path is verified on any host, including Metal
/// builds where `omegaSLStructStride` itself never takes the std140 branch.

#include "../src/BufferIO.h"

#include <cassert>
#include <cstdio>

using namespace OmegaGTE;
using Std = BufferLayoutStd;

// --- Compile-time: matrix column stride. std140 forces every column to 16;
//     std430 uses the column's vecR alignment (vec1=4, vec2=8, vec3/4=16). ---
static_assert(matrixColumnStride(1, Std::Std430) == 4, "std430 mat*x1 column = 4");
static_assert(matrixColumnStride(2, Std::Std430) == 8, "std430 mat*x2 column = 8");
static_assert(matrixColumnStride(3, Std::Std430) == 16, "std430 mat*x3 column = 16 (vec3 padded)");
static_assert(matrixColumnStride(4, Std::Std430) == 16, "std430 mat*x4 column = 16");
static_assert(matrixColumnStride(1, Std::Std140) == 16, "std140 column always 16");
static_assert(matrixColumnStride(2, Std::Std140) == 16, "std140 column always 16");
static_assert(matrixColumnStride(3, Std::Std140) == 16, "std140 column always 16");
static_assert(matrixColumnStride(4, Std::Std140) == 16, "std140 column always 16");

// --- Compile-time: full matrix size. Diverges only where std430 columns are
//     narrower than 16 (mat*x1, mat*x2). ---
static_assert(matrixSize(4, 4, Std::Std430) == 64 && matrixSize(4, 4, Std::Std140) == 64,
              "float4x4 = 64 in both");
static_assert(matrixSize(4, 3, Std::Std430) == 64 && matrixSize(4, 3, Std::Std140) == 64,
              "float4x3 = 64 in both (vec3 columns padded to 16)");
static_assert(matrixSize(2, 2, Std::Std430) == 16, "float2x2 std430 = 2*8");
static_assert(matrixSize(2, 2, Std::Std140) == 32, "float2x2 std140 = 2*16");
static_assert(matrixSize(3, 2, Std::Std430) == 24, "float3x2 std430 = 3*8");
static_assert(matrixSize(3, 2, Std::Std140) == 48, "float3x2 std140 = 3*16");

// --- DirectX StructuredBuffer layout (BufferLayoutStd::DXStructured) — the
//     scalar-aligned layout the D3D12 backend packs storage buffers in. Every
//     member aligns to its 4-byte component (NO std430 vec3->16 column pad), so
//     matrix columns pack tightly at R*4 (float3x3 = 36, not 48). float*x4
//     columns are 16 either way, which is why float4x4 round-trips under either
//     layout while float3x3 does not. Reference values per the DXIL rules. ---
static_assert(matrixColumnStride(1, Std::DXStructured) == 4,  "DX mat*x1 column = 4");
static_assert(matrixColumnStride(2, Std::DXStructured) == 8,  "DX mat*x2 column = 8");
static_assert(matrixColumnStride(3, Std::DXStructured) == 12, "DX mat*x3 column = 12 (tight, no pad)");
static_assert(matrixColumnStride(4, Std::DXStructured) == 16, "DX mat*x4 column = 16");
static_assert(matrixSize(3, 3, Std::DXStructured) == 36, "float3x3 DX = 3*12");
static_assert(matrixSize(2, 2, Std::DXStructured) == 16, "float2x2 DX = 2*8");
static_assert(matrixSize(4, 4, Std::DXStructured) == 64, "float4x4 DX = 4*16 (matches std430)");
static_assert(matrixSize(3, 4, Std::DXStructured) == 48, "float3x4 DX = 3*16");
static_assert(matrixSize(4, 3, Std::DXStructured) == 48, "float4x3 DX = 4*12");

int main() {
    // std140 struct stride: align each member, then round the struct to 16.
    assert(std140StructStride({OMEGASL_FLOAT4}) == 16);
    assert(std140StructStride({OMEGASL_FLOAT}) == 16);                       // 4 -> 16
    assert(std140StructStride({OMEGASL_FLOAT, OMEGASL_FLOAT}) == 16);        // 8 -> 16
    assert(std140StructStride({OMEGASL_FLOAT4, OMEGASL_FLOAT4}) == 32);
    assert(std140StructStride({OMEGASL_FLOAT2, OMEGASL_FLOAT2}) == 16);      // 8+8
    assert(std140StructStride({OMEGASL_FLOAT3, OMEGASL_FLOAT}) == 16);       // vec3(12)+float(4) packs to 16
    assert(std140StructStride({OMEGASL_FLOAT4x4, OMEGASL_FLOAT4, OMEGASL_FLOAT}) == 96); // 64+16+4 -> 96
    assert(std140StructStride({OMEGASL_FLOAT, OMEGASL_FLOAT2x2}) == 48);     // float@0; mat2x2 align16 @16 size32 -> 48
    assert(std140StructStride({OMEGASL_FLOAT4, OMEGASL_FLOAT3x2}) == 64);    // vec4@0; mat3x2 align16 @16 size48 -> 64

    // The same struct is tighter under std430 wherever a narrow matrix or a
    // sub-16 tail is involved — proves the two standards actually diverge.
    assert(matrixSize(2, 2, Std::Std140) > matrixSize(2, 2, Std::Std430));

    // --- §2.4-1: per-member base alignment (shared by every backend's
    //     align-then-place writer/reader). Standard-driven, identical for
    //     scalar/vector across both standards; matrices follow the column
    //     alignment (std430: vecR; std140: always 16). ---
    assert(memberBaseAlignment(OMEGASL_FLOAT,  Std::Std430) == 4);
    assert(memberBaseAlignment(OMEGASL_FLOAT2, Std::Std430) == 8);
    assert(memberBaseAlignment(OMEGASL_FLOAT3, Std::Std430) == 16);
    assert(memberBaseAlignment(OMEGASL_FLOAT4, Std::Std430) == 16);
    assert(memberBaseAlignment(OMEGASL_INT3,   Std::Std140) == 16);
    assert(memberBaseAlignment(OMEGASL_FLOAT2x2, Std::Std430) == 8);   // vec2 columns
    assert(memberBaseAlignment(OMEGASL_FLOAT2x2, Std::Std140) == 16);  // forced to 16
    assert(memberBaseAlignment(OMEGASL_FLOAT4x4, Std::Std430) == 16);

    // DirectX StructuredBuffer: every member aligns to its 4-byte component,
    // matrices included — the legacy 16-byte column/struct rule is cbuffer-only.
    assert(memberBaseAlignment(OMEGASL_FLOAT,    Std::DXStructured) == 4);
    assert(memberBaseAlignment(OMEGASL_FLOAT2,   Std::DXStructured) == 4);
    assert(memberBaseAlignment(OMEGASL_FLOAT3,   Std::DXStructured) == 4);
    assert(memberBaseAlignment(OMEGASL_FLOAT4,   Std::DXStructured) == 4);
    assert(memberBaseAlignment(OMEGASL_FLOAT3x3, Std::DXStructured) == 4);
    assert(memberBaseAlignment(OMEGASL_FLOAT2x2, Std::DXStructured) == 4);
    // DX struct stride: scalar-aligned, no final 16-byte rounding (matches DXIL
    // StructureByteStride). The two canonical reference cases from the layout
    // rules: {float, float4} = 20 (float4 @4, not @16); {float4, float3x3} = 52.
    assert(dxStructuredStructStride({OMEGASL_FLOAT, OMEGASL_FLOAT4}) == 20);
    assert(dxStructuredStructStride({OMEGASL_FLOAT4, OMEGASL_FLOAT3x3}) == 52);
    // The matrix_ops_test structs: MatIn {float4x4,float3x3,float2x2,float4} and
    // the mixed {float,float4,float2,float4x4,float} — host must size these to
    // the DX layout so the GPU's StructuredBuffer reads line up.
    assert(dxStructuredStructStride(
        {OMEGASL_FLOAT4x4, OMEGASL_FLOAT3x3, OMEGASL_FLOAT2x2, OMEGASL_FLOAT4}) == 132);
    assert(dxStructuredStructStride(
        {OMEGASL_FLOAT, OMEGASL_FLOAT4, OMEGASL_FLOAT2, OMEGASL_FLOAT4x4, OMEGASL_FLOAT}) == 96);

    // alignOffset rounds up to the next multiple (no-op when already aligned).
    assert(alignOffset(0, 16) == 0);
    assert(alignOffset(4, 16) == 16);
    assert(alignOffset(16, 16) == 16);
    assert(alignOffset(12, 8) == 16);

    // --- §2.4-1: std430 flat-struct stride (logical, vec3 = 12), the layout
    //     the Vulkan / D3D12 writers/readers pack to. The 16-aligned common
    //     case is unchanged from the prior heuristic; mixed sub-16 orders are
    //     the cases the heuristic got wrong. ---
    assert(std430StructStride({OMEGASL_FLOAT4}) == 16);
    assert(std430StructStride({OMEGASL_FLOAT4, OMEGASL_FLOAT4}) == 32);
    assert(std430StructStride({OMEGASL_FLOAT4, OMEGASL_FLOAT2, OMEGASL_FLOAT2}) == 32);
    assert(std430StructStride({OMEGASL_FLOAT}) == 4);                         // scalar struct, align 4
    assert(std430StructStride({OMEGASL_FLOAT, OMEGASL_FLOAT}) == 8);
    // Mixed sub-16 orders — float@0, float4 forced to 16 -> ends 32 (align 16).
    assert(std430StructStride({OMEGASL_FLOAT, OMEGASL_FLOAT4}) == 32);
    // vec2 then float4x4: vec2@0(8), mat@16(64) -> 80 (align 16).
    assert(std430StructStride({OMEGASL_FLOAT2, OMEGASL_FLOAT4x4}) == 80);
    // float4 then float2: ends at 24, struct align 16 -> 32.
    assert(std430StructStride({OMEGASL_FLOAT4, OMEGASL_FLOAT2}) == 32);
    // vec3(12) + float(4) pack tight to 16 (no gap: float align 4 lands at 12).
    assert(std430StructStride({OMEGASL_FLOAT3, OMEGASL_FLOAT}) == 16);
    // The matrix-ops layout: all members already 16-aligned, so std430 and the
    // earlier heuristic agree on the data extent (144).
    assert(std430StructStride({OMEGASL_FLOAT4x4, OMEGASL_FLOAT3x3,
                               OMEGASL_FLOAT2x2, OMEGASL_FLOAT4}) == 144);

    // The same mixed order is wider under std140 (struct rounds to 16 and
    // matrix columns are 16-strided) — diverges from std430 where it matters.
    assert(std140StructStride({OMEGASL_FLOAT2, OMEGASL_FLOAT2x2})
           > std430StructStride({OMEGASL_FLOAT2, OMEGASL_FLOAT2x2}));

    // --- §12.2 follow-up: integer / unsigned matrices share the float layout
    //     (int/uint/float are all 4-byte scalars), so matrixDims and every
    //     stride must match the same-shape float matrix exactly. ---
    assert(matrixDims(OMEGASL_INT4x4) == std::make_pair(4u, 4u));
    assert(matrixDims(OMEGASL_UINT3x2) == std::make_pair(3u, 2u));
    assert(matrixDims(OMEGASL_INT2x3) == std::make_pair(2u, 3u));
    assert(isMatrixDataType(OMEGASL_INT4x4) && isMatrixDataType(OMEGASL_UINT2x2));

    assert(std140StructStride({OMEGASL_INT4x4}) == std140StructStride({OMEGASL_FLOAT4x4}));
    assert(std140StructStride({OMEGASL_UINT3x2}) == std140StructStride({OMEGASL_FLOAT3x2}));
    assert(std140StructStride({OMEGASL_INT, OMEGASL_INT2x2})
           == std140StructStride({OMEGASL_FLOAT, OMEGASL_FLOAT2x2}));
    // Mixed int/float struct stride matches an all-float struct of the same shapes.
    assert(std140StructStride({OMEGASL_INT4x4, OMEGASL_UINT4, OMEGASL_INT})
           == std140StructStride({OMEGASL_FLOAT4x4, OMEGASL_FLOAT4, OMEGASL_FLOAT}));

    std::printf("std140 layout test passed\n");
    return 0;
}
