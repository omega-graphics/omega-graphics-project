/// OmegaSL §5.2 / §12 — matrix-op GPU integration test. Backend-independent:
/// uses only the OmegaGTE public API + runtime OmegaSL compilation, so the
/// same source builds and runs on Metal, Vulkan, and D3D12 (mirrors
/// sampler_bind_test.cpp). Headless — a single compute dispatch, then a buffer
/// readback.
///
/// One compute kernel reads three square input matrices (2x2 / 3x3 / 4x4) and
/// a vector from a buffer, exercises every matrix op OmegaSL exposes, and
/// writes the results to an output buffer the host reads back. The checks are
/// deliberately *convention-free* so they don't depend on the host-vs-shader
/// row/column-major mapping — each asserts an algebraic identity that holds in
/// any consistent convention:
///
///   inverse:      inverse(A) * A == I   and   A * inverse(A) == I   (2x2/3x3/4x4)
///   multiply:     A * I == A            (covered by the matmul in the inverse
///                                         checks, plus an explicit identity mul)
///   matrix*vector: inverse(A) * (A * v) == v
///   transpose:    transpose(transpose(A)) == A   (involution)
///                 and transpose(A)[c][r] == A[r][c]  (index-swap — the one
///                 check tied to the storage convention; see note below)
///   determinant:  determinant(A) == host-computed det (transpose-invariant)
///
/// Buffer layout note (§2.4-1): the matrix-op structs above are all
/// 16-byte-aligned (matrices and float4 only — the three scalar determinants
/// are packed into one float4). A second kernel (`mixedLayout`) deliberately
/// uses a struct whose field order interleaves sub-16 scalars/vectors with
/// larger members (scalar→float4, float2→float4x4, trailing scalar) to
/// exercise the per-member align-then-place padding the writers/readers gained
/// in §2.4-1 — it round-trips only if that padding is correct.

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GTEMath.h>
#include <omegaGTE/GTEShader.h>
#include "GTETestEntryPoint.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace OmegaGTE;

namespace {

OmegaCommon::String kShaders = R"(

struct MatIn {
    float4x4 a4;
    float3x3 a3;
    float2x2 a2;
    float4   v;
};

struct MatOut {
    float4x4 inv4_lhs;     // inverse(a4) * a4   -> I4
    float4x4 inv4_rhs;     // a4 * inverse(a4)   -> I4
    float4x4 mul4_id;      // a4 * I4            -> a4
    float4x4 tpose4_once;  // transpose(a4)      -> a4 with indices swapped
    float4x4 tpose4_rt;    // transpose(transpose(a4)) -> a4
    float3x3 inv3_lhs;     // inverse(a3) * a3   -> I3
    float2x2 inv2_lhs;     // inverse(a2) * a2   -> I2
    float4   matvec_rt;    // inverse(a4) * (a4 * v) -> v
    float4   dets;         // (det(a2), det(a3), det(a4), 0)
};

buffer<MatIn>  inBuf  : 0;
buffer<MatOut> outBuf : 1;

[in inBuf, out outBuf]
compute(x=1,y=1,z=1)
void matrixOps(uint3 tid : GlobalThreadID){
    float4x4 a4 = inBuf[0].a4;
    float3x3 a3 = inBuf[0].a3;
    float2x2 a2 = inBuf[0].a2;
    float4   v  = inBuf[0].v;

    float4x4 id4 = float4x4(
        1.0,0.0,0.0,0.0,
        0.0,1.0,0.0,0.0,
        0.0,0.0,1.0,0.0,
        0.0,0.0,0.0,1.0);

    outBuf[0].inv4_lhs    = inverse(a4) * a4;
    outBuf[0].inv4_rhs    = a4 * inverse(a4);
    outBuf[0].mul4_id     = a4 * id4;
    outBuf[0].tpose4_once = transpose(a4);
    outBuf[0].tpose4_rt   = transpose(transpose(a4));
    outBuf[0].inv3_lhs    = inverse(a3) * a3;
    outBuf[0].inv2_lhs    = inverse(a2) * a2;
    outBuf[0].matvec_rt   = inverse(a4) * (a4 * v);
    outBuf[0].dets        = float4(determinant(a2), determinant(a3), determinant(a4), 0.0);
}

// §2.4-1 — mixed-order buffer-layout round-trip. The field order interleaves
// sub-16 scalars/vectors with larger members (scalar before float4, float2
// before float4x4, trailing scalar), so the struct only round-trips if the
// GEBufferWriter/Reader insert correct inter-member padding. Each field is
// doubled so the GPU must actually read it from the right offset (a misaligned
// read yields garbage, not 2x). The matrix is echoed unchanged (matrix*scalar
// / matrix+matrix aren't part of the verified op surface).
struct MixIO {
    float    s0;   // @0
    float4   v0;   // must align 16 -> @16 (12-byte gap)
    float2   v1;   // @32
    float4x4 m0;   // must align 16 -> @48 (8-byte gap)
    float    s1;   // @112 (trailing sub-16)
};

buffer<MixIO> mixInBuf  : 0;
buffer<MixIO> mixOutBuf : 1;

[in mixInBuf, out mixOutBuf]
compute(x=1,y=1,z=1)
void mixedLayout(uint3 tid : GlobalThreadID){
    mixOutBuf[0].s0 = mixInBuf[0].s0 + mixInBuf[0].s0;
    mixOutBuf[0].v0 = mixInBuf[0].v0 + mixInBuf[0].v0;
    mixOutBuf[0].v1 = mixInBuf[0].v1 + mixInBuf[0].v1;
    mixOutBuf[0].m0 = mixInBuf[0].m0;
    mixOutBuf[0].s1 = mixInBuf[0].s1 + mixInBuf[0].s1;
}

)";

// The input matrices, in host m[c][r] (column-major) indexing. Diagonally
// dominant so each is well-conditioned (a stable inverse) and asymmetric so a
// stray transpose would change a readback element.
constexpr float kA4[4][4] = {  // [col][row]
    {5, 1, 2, 0},
    {0, 6, 1, 3},
    {1, 0, 7, 2},
    {2, 1, 0, 8},
};
constexpr float kA3[3][3] = {  // [col][row]
    {4, 1, 0},
    {0, 5, 2},
    {1, 0, 6},
};
constexpr float kA2[2][2] = {  // [col][row]
    {3, 1},
    {0, 2},
};
constexpr float kV[4] = {1, 2, 3, 4};

// Determinant of an NxN grid by Laplace expansion. det is transpose-invariant,
// so it equals the GPU's determinant regardless of the row/col convention.
double detGrid(const std::vector<std::vector<double>> &m) {
    size_t n = m.size();
    if (n == 1) return m[0][0];
    if (n == 2) return m[0][0] * m[1][1] - m[0][1] * m[1][0];
    double acc = 0.0;
    int sign = 1;
    for (size_t k = 0; k < n; ++k) {
        std::vector<std::vector<double>> sub;
        for (size_t i = 1; i < n; ++i) {
            std::vector<double> row;
            for (size_t j = 0; j < n; ++j)
                if (j != k) row.push_back(m[i][j]);
            sub.push_back(row);
        }
        acc += sign * m[0][k] * detGrid(sub);
        sign = -sign;
    }
    return acc;
}

template <size_t N>
double detOf(const float a[N][N]) {
    std::vector<std::vector<double>> g(N, std::vector<double>(N));
    for (size_t i = 0; i < N; ++i)
        for (size_t j = 0; j < N; ++j) g[i][j] = a[i][j];
    return detGrid(g);
}

bool approx(float got, float want, float tol = 1e-3f) {
    return std::fabs(got - want) <= tol;
}

bool &failFlag() { static bool f = false; return f; }

template <unsigned C, unsigned R>
void expectMatrix(const char *name, FMatrix<C, R> &got,
                  const std::function<float(unsigned, unsigned)> &want, float tol = 1e-3f) {
    for (unsigned c = 0; c < C; ++c)
        for (unsigned r = 0; r < R; ++r) {
            if (!approx(got[c][r], want(c, r), tol)) {
                std::cerr << "  FAIL " << name << "[" << c << "][" << r << "] = " << got[c][r]
                          << " expected " << want(c, r) << "\n";
                failFlag() = true;
            }
        }
}

}  // namespace

GTE_TEST_ENTRY_POINT {
    (void)argc;
    (void)argv;
    auto gte = OmegaGTE::InitWithDefaultDevice();

    auto compiled = gte.omegaSlCompiler->compile({OmegaSLCompiler::Source::fromString(kShaders)});
    auto lib = gte.graphicsEngine->loadShaderLibraryRuntime(compiled);

    ComputePipelineDescriptor pd{};
    pd.computeFunc = lib->shaders["matrixOps"];
    if (!pd.computeFunc) {
        std::cerr << "matrixOps shader not found\n";
        OmegaGTE::Close(gte);
        return 1;
    }
    auto pipeline = gte.graphicsEngine->makeComputePipelineState(pd);

    const auto inLayout = OmegaCommon::Vector<omegasl_data_type>{
        OMEGASL_FLOAT4x4, OMEGASL_FLOAT3x3, OMEGASL_FLOAT2x2, OMEGASL_FLOAT4};
    const auto outLayout = OmegaCommon::Vector<omegasl_data_type>{
        OMEGASL_FLOAT4x4, OMEGASL_FLOAT4x4, OMEGASL_FLOAT4x4, OMEGASL_FLOAT4x4,
        OMEGASL_FLOAT4x4, OMEGASL_FLOAT3x3, OMEGASL_FLOAT2x2, OMEGASL_FLOAT4,
        OMEGASL_FLOAT4};
    const size_t inSize = omegaSLStructStride(inLayout);
    const size_t outSize = omegaSLStructStride(outLayout);

    auto inBuf = gte.graphicsEngine->makeBuffer({BufferDescriptor::Upload, inSize, inSize});
    auto outBuf = gte.graphicsEngine->makeBuffer({BufferDescriptor::Upload, outSize, outSize});

    // Write the input matrices.
    auto a4 = FMatrix<4, 4>::Create();
    for (unsigned c = 0; c < 4; ++c) for (unsigned r = 0; r < 4; ++r) a4[c][r] = kA4[c][r];
    auto a3 = FMatrix<3, 3>::Create();
    for (unsigned c = 0; c < 3; ++c) for (unsigned r = 0; r < 3; ++r) a3[c][r] = kA3[c][r];
    auto a2 = FMatrix<2, 2>::Create();
    for (unsigned c = 0; c < 2; ++c) for (unsigned r = 0; r < 2; ++r) a2[c][r] = kA2[c][r];
    auto vv = FVec<4>::Create();
    for (unsigned i = 0; i < 4; ++i) vv[i][0] = kV[i];

    auto writer = GEBufferWriter::Create();
    writer->setOutputBuffer(inBuf);
    writer->structBegin();
    writer->writeFloat4x4(a4);
    writer->writeFloat3x3(a3);
    writer->writeFloat2x2(a2);
    writer->writeFloat4(vv);
    writer->structEnd();
    writer->sendToBuffer();
    writer->flush();

    // Dispatch.
    OmegaGTE::GECommandQueueDesc queueDesc{};
    queueDesc.maxBufferCount = 1;
    auto queue = gte.graphicsEngine->makeCommandQueue(queueDesc);
    auto cmd = queue->getAvailableBuffer();
    GEComputePassDescriptor pass{};
    cmd->startComputePass(pass);
    cmd->setComputePipelineState(pipeline);
    cmd->bindResourceAtComputeShader(inBuf, 0);
    cmd->bindResourceAtComputeShader(outBuf, 1);
    cmd->dispatchThreads(1, 1, 1);
    cmd->finishComputePass();
    queue->submitCommandBuffer(cmd);
    queue->commitToGPUAndWait();

    // Read back.
    auto reader = GEBufferReader::Create();
    reader->setInputBuffer(outBuf);
    reader->setStructLayout(outLayout);
    reader->structBegin();
    auto inv4_lhs = FMatrix<4, 4>::Create();
    auto inv4_rhs = FMatrix<4, 4>::Create();
    auto mul4_id = FMatrix<4, 4>::Create();
    auto tpose4_once = FMatrix<4, 4>::Create();
    auto tpose4_rt = FMatrix<4, 4>::Create();
    auto inv3_lhs = FMatrix<3, 3>::Create();
    auto inv2_lhs = FMatrix<2, 2>::Create();
    auto matvec_rt = FVec<4>::Create();
    auto dets = FVec<4>::Create();
    reader->getFloat4x4(inv4_lhs);
    reader->getFloat4x4(inv4_rhs);
    reader->getFloat4x4(mul4_id);
    reader->getFloat4x4(tpose4_once);
    reader->getFloat4x4(tpose4_rt);
    reader->getFloat3x3(inv3_lhs);
    reader->getFloat2x2(inv2_lhs);
    reader->getFloat4(matvec_rt);
    reader->getFloat4(dets);
    reader->structEnd();
    reader->reset();

    auto identity = [](unsigned c, unsigned r) { return c == r ? 1.0f : 0.0f; };

    // inverse(A) * A == I  and  A * inverse(A) == I.
    expectMatrix<4, 4>("inverse(a4)*a4", inv4_lhs, identity);
    expectMatrix<4, 4>("a4*inverse(a4)", inv4_rhs, identity);
    expectMatrix<3, 3>("inverse(a3)*a3", inv3_lhs, identity);
    expectMatrix<2, 2>("inverse(a2)*a2", inv2_lhs, identity);

    // A * I == A (matrix multiply preserves through an identity right-operand).
    expectMatrix<4, 4>("a4*I4", mul4_id, [](unsigned c, unsigned r) { return kA4[c][r]; });

    // transpose(transpose(A)) == A (involution).
    expectMatrix<4, 4>("transpose^2(a4)", tpose4_rt, [](unsigned c, unsigned r) { return kA4[c][r]; });

    // transpose(A)[c][r] == A[r][c]. The single convention-dependent check;
    // verified on Metal here, run on D3D12 / Vulkan to confirm the others.
    expectMatrix<4, 4>("transpose(a4)", tpose4_once, [](unsigned c, unsigned r) { return kA4[r][c]; });

    // inverse(A) * (A * v) == v.
    for (unsigned i = 0; i < 4; ++i) {
        if (!approx(matvec_rt[i][0], kV[i])) {
            std::cerr << "  FAIL matvec[" << i << "] = " << matvec_rt[i][0]
                      << " expected " << kV[i] << "\n";
            failFlag() = true;
        }
    }

    // determinant — transpose-invariant, compared against a host computation.
    const float det2 = (float)detOf<2>(kA2);
    const float det3 = (float)detOf<3>(kA3);
    const float det4 = (float)detOf<4>(kA4);
    auto detTol = [](float v) { return std::fmax(1e-2f, 1e-3f * std::fabs(v)); };
    if (!approx(dets[0][0], det2, detTol(det2))) {
        std::cerr << "  FAIL det(a2) = " << dets[0][0] << " expected " << det2 << "\n";
        failFlag() = true;
    }
    if (!approx(dets[1][0], det3, detTol(det3))) {
        std::cerr << "  FAIL det(a3) = " << dets[1][0] << " expected " << det3 << "\n";
        failFlag() = true;
    }
    if (!approx(dets[2][0], det4, detTol(det4))) {
        std::cerr << "  FAIL det(a4) = " << dets[2][0] << " expected " << det4 << "\n";
        failFlag() = true;
    }

    // ------------------------------------------------------------------
    // §2.4-1 — mixed-order buffer-layout round-trip on the GPU.
    // ------------------------------------------------------------------
    ComputePipelineDescriptor mpd{};
    mpd.computeFunc = lib->shaders["mixedLayout"];
    if (!mpd.computeFunc) {
        std::cerr << "mixedLayout shader not found\n";
        OmegaGTE::Close(gte);
        return 1;
    }
    auto mixPipeline = gte.graphicsEngine->makeComputePipelineState(mpd);

    // Field order: float, float4, float2, float4x4, float — interleaves sub-16
    // members with larger ones, so it only round-trips with correct
    // inter-member padding.
    const auto mixLayout = OmegaCommon::Vector<omegasl_data_type>{
        OMEGASL_FLOAT, OMEGASL_FLOAT4, OMEGASL_FLOAT2, OMEGASL_FLOAT4x4, OMEGASL_FLOAT};
    const size_t mixSize = omegaSLStructStride(mixLayout);

    auto mixIn = gte.graphicsEngine->makeBuffer({BufferDescriptor::Upload, mixSize, mixSize});
    auto mixOut = gte.graphicsEngine->makeBuffer({BufferDescriptor::Upload, mixSize, mixSize});

    // Known inputs. Distinct values per field so a wrong offset surfaces.
    float mS0 = 1.5f;
    auto mV0 = FVec<4>::Create();
    const float kMV0[4] = {2, 3, 4, 5};
    for (unsigned i = 0; i < 4; ++i) mV0[i][0] = kMV0[i];
    auto mV1 = FVec<2>::Create();
    const float kMV1[2] = {6, 7};
    for (unsigned i = 0; i < 2; ++i) mV1[i][0] = kMV1[i];
    auto mM0 = FMatrix<4, 4>::Create();
    for (unsigned c = 0; c < 4; ++c) for (unsigned r = 0; r < 4; ++r) mM0[c][r] = kA4[c][r];
    float mS1 = 9.5f;

    auto mWriter = GEBufferWriter::Create();
    mWriter->setOutputBuffer(mixIn);
    mWriter->structBegin();
    mWriter->writeFloat(mS0);
    mWriter->writeFloat4(mV0);
    mWriter->writeFloat2(mV1);
    mWriter->writeFloat4x4(mM0);
    mWriter->writeFloat(mS1);
    mWriter->structEnd();
    mWriter->sendToBuffer();
    mWriter->flush();

    auto mCmd = queue->getAvailableBuffer();
    GEComputePassDescriptor mPass{};
    mCmd->startComputePass(mPass);
    mCmd->setComputePipelineState(mixPipeline);
    mCmd->bindResourceAtComputeShader(mixIn, 0);
    mCmd->bindResourceAtComputeShader(mixOut, 1);
    mCmd->dispatchThreads(1, 1, 1);
    mCmd->finishComputePass();
    queue->submitCommandBuffer(mCmd);
    queue->commitToGPUAndWait();

    auto mReader = GEBufferReader::Create();
    mReader->setInputBuffer(mixOut);
    mReader->setStructLayout(mixLayout);
    mReader->structBegin();
    float oS0 = 0.f;
    auto oV0 = FVec<4>::Create();
    auto oV1 = FVec<2>::Create();
    auto oM0 = FMatrix<4, 4>::Create();
    float oS1 = 0.f;
    mReader->getFloat(oS0);
    mReader->getFloat4(oV0);
    mReader->getFloat2(oV1);
    mReader->getFloat4x4(oM0);
    mReader->getFloat(oS1);
    mReader->structEnd();
    mReader->reset();

    // Each scalar/vector is doubled by the kernel; the matrix is echoed.
    if (!approx(oS0, mS0 * 2.f)) {
        std::cerr << "  FAIL mixed.s0 = " << oS0 << " expected " << mS0 * 2.f << "\n";
        failFlag() = true;
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (!approx(oV0[i][0], kMV0[i] * 2.f)) {
            std::cerr << "  FAIL mixed.v0[" << i << "] = " << oV0[i][0]
                      << " expected " << kMV0[i] * 2.f << "\n";
            failFlag() = true;
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (!approx(oV1[i][0], kMV1[i] * 2.f)) {
            std::cerr << "  FAIL mixed.v1[" << i << "] = " << oV1[i][0]
                      << " expected " << kMV1[i] * 2.f << "\n";
            failFlag() = true;
        }
    }
    expectMatrix<4, 4>("mixed.m0", oM0, [](unsigned c, unsigned r) { return kA4[c][r]; });
    if (!approx(oS1, mS1 * 2.f)) {
        std::cerr << "  FAIL mixed.s1 = " << oS1 << " expected " << mS1 * 2.f << "\n";
        failFlag() = true;
    }

    OmegaGTE::Close(gte);

    const bool ok = !failFlag();
    std::cout << (ok ? "PASS: matrix ops" : "FAIL: matrix ops") << "\n";
    return ok ? 0 : 1;
}
