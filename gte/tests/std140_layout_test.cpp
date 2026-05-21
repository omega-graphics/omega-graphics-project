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

    std::printf("std140 layout test passed\n");
    return 0;
}
