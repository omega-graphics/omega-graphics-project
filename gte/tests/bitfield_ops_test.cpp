/// OmegaSL §5.3 — integer / bitfield-op GPU integration test.
/// Backend-independent: uses only the OmegaGTE public API + runtime OmegaSL
/// compilation, so the same source builds and runs on Metal, Vulkan, and
/// D3D12 (mirrors matrix_ops_test.cpp). Headless — one compute dispatch then
/// a buffer readback.
///
/// The point of this test (beyond "it compiles") is to prove the §5.3 Phase B
/// firstbit *normalization* actually agrees with the host contract on real
/// hardware: `firstbithigh`/`firstbitlow` must return the zero-based bit index
/// of the highest/lowest set bit, or -1 when no bit is set — identically on
/// every backend. The per-backend lowerings differ wildly (HLSL
/// `firstbit*`+0xFFFFFFFF→-1, GLSL native findMSB/findLSB, MSL clz/ctz with a
/// zero-select), so a GPU readback is the only check that the three converge.
/// Phase A `countbits`/`reversebits` are exercised too.
///
/// The kernel reads a uint4 of test operands and writes float4 results (the
/// firstbit / popcount values are small integers, exact in float), so the
/// readback needs only `getFloat4`. (A direct integer readback is now possible
/// via the reader's `getInt*` / `getUint*` API — exercised by
/// int_vector_io_test.cpp — but this test keeps the float form: comparing
/// reversebits against the host expectation on-GPU avoids round-tripping a
/// full-range uint through a lossy float.)

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GTEShader.h>
#include "GTETestEntryPoint.h"

#include <cstdint>
#include <iostream>
#include <vector>

using namespace OmegaGTE;

namespace {

OmegaCommon::String kShaders = R"(

struct InData  {
    uint4 ops;    // operands for firstbit / countbits / bitfield
    uint4 rexp;   // host-computed reversebits(ops) expected values
    uint4 bfexp;  // host-computed (extractU, extractS, insert, 0) for ops.x
};
// highs/lows/counts/revOK as before, plus bfOK for the §5.3-C extract/insert.
struct OutData {
    float4 highs;     // firstbithigh(op_i)
    float4 lows;      // firstbitlow(op_i)
    float4 counts;    // countbits(op_i)
    float4 revOK;     // 1.0 if reversebits(op_i) == host expected, else 0.0
    float4 bfOK;      // (extractU, extractS, insert, 1) each 1.0 if == host
};

buffer<InData>  inBuf  : 0;
buffer<OutData> outBuf : 1;

[in inBuf, out outBuf]
compute(x=1,y=1,z=1)
void bitfieldOps(uint3 tid : GlobalThreadID){
    uint4 op = inBuf[0].ops;

    int4 hi = firstbithigh(op);   // normalized: index or -1
    int4 lo = firstbitlow(op);
    uint4 cb = countbits(op);
    uint4 rb = reversebits(op);   // compared against host expected, on-GPU,
                                  // so the full-range uint value never has to
                                  // round-trip through a lossy float.

    outBuf[0].highs  = float4(float(hi.x), float(hi.y), float(hi.z), float(hi.w));
    outBuf[0].lows   = float4(float(lo.x), float(lo.y), float(lo.z), float(lo.w));
    outBuf[0].counts = float4(float(cb.x), float(cb.y), float(cb.z), float(cb.w));
    uint4 re = inBuf[0].rexp;
    outBuf[0].revOK  = float4(float(rb.x == re.x ? 1u : 0u),
                              float(rb.y == re.y ? 1u : 0u),
                              float(rb.z == re.z ? 1u : 0u),
                              float(rb.w == re.w ? 1u : 0u));

    // §5.3-C — extract/insert on a known operand (ops.x = 0xA5A5A5A5),
    // offset 4, 8 bits. Compared against host-computed expectations on-GPU.
    uint  base = op.x;
    uint  eu = bitfieldExtract(base, 4, 8);             // unsigned, zero-extend
    int   es = bitfieldExtract((int)base, 4, 8);        // signed,   sign-extend
    uint  ib = bitfieldInsert(base, 0xFFu, 4, 8);       // insert low 8 bits
    uint4 bfe = inBuf[0].bfexp;
    outBuf[0].bfOK = float4(float(eu == bfe.x ? 1u : 0u),
                            float((uint)es == bfe.y ? 1u : 0u),
                            float(ib == bfe.z ? 1u : 0u),
                            1.0);
}

)";

// Test operands and their expected normalized firstbit / popcount results.
// Chosen to hit the edges: 0 (no bits -> -1), 1 (lsb), 0x80000000 (msb),
// 0xFFFFFFFF (all bits).
static uint32_t reverseBits32(uint32_t x) {
    uint32_t r = 0;
    for (int i = 0; i < 32; ++i) { r = (r << 1) | (x & 1u); x >>= 1; }
    return r;
}

// Host reference for bitfieldExtract / bitfieldInsert (the GLSL/MSL spec the
// HLSL manual lowering must match). bits>0, off+bits<=32 here.
static uint32_t bfExtractU(uint32_t v, int off, int bits) {
    uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    return (v >> off) & mask;
}
static int32_t bfExtractS(int32_t v, int off, int bits) {
    uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    uint32_t raw = ((uint32_t)v >> off) & mask;
    uint32_t sign = 1u << (bits - 1);
    if (raw & sign) raw |= ~mask;
    return (int32_t)raw;
}
static uint32_t bfInsert(uint32_t base, uint32_t ins, int off, int bits) {
    uint32_t mask = ((bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u)) << off;
    return (base & ~mask) | ((ins << off) & mask);
}

struct Case { uint32_t v; int high; int low; int count; };
constexpr Case kCases[4] = {
    {0x00000000u, -1, -1, 0},
    {0x00000001u,  0,  0, 1},
    {0x80000000u, 31, 31, 1},
    {0xFFFFFFFFu, 31,  0, 32},
};

bool &failFlag() { static bool f = false; return f; }

void expectF(const char *name, int idx, float got, float want) {
    if (got != want) {
        std::cerr << "  FAIL " << name << "[" << idx << "] = " << got
                  << " expected " << want << "\n";
        failFlag() = true;
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
    pd.computeFunc = lib->shaders["bitfieldOps"];
    if (!pd.computeFunc) {
        std::cerr << "bitfieldOps shader not found\n";
        OmegaGTE::Close(gte);
        return 1;
    }
    auto pipeline = gte.graphicsEngine->makeComputePipelineState(pd);

    const auto inLayout = OmegaCommon::Vector<omegasl_data_type>{
        OMEGASL_UINT4, OMEGASL_UINT4, OMEGASL_UINT4};
    const auto outLayout = OmegaCommon::Vector<omegasl_data_type>{
        OMEGASL_FLOAT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4};
    const size_t inSize = omegaSLStructStride(inLayout);
    const size_t outSize = omegaSLStructStride(outLayout);

    auto inBuf = gte.graphicsEngine->makeBuffer({BufferDescriptor::Upload, inSize, inSize});
    auto outBuf = gte.graphicsEngine->makeBuffer({BufferDescriptor::Upload, outSize, outSize});

    // Write the four operands plus the host-computed reversebits expectations.
    auto ops = UVec<4>::Create();
    auto rexp = UVec<4>::Create();
    for (unsigned i = 0; i < 4; ++i) {
        ops[i][0] = kCases[i].v;
        rexp[i][0] = reverseBits32(kCases[i].v);
    }
    // bitfield extract/insert expectations on ops.x, offset 4, 8 bits.
    auto bfexp = UVec<4>::Create();
    bfexp[0][0] = bfExtractU(kCases[0].v, 4, 8);
    bfexp[1][0] = (uint32_t)bfExtractS((int32_t)kCases[0].v, 4, 8);
    bfexp[2][0] = bfInsert(kCases[0].v, 0xFFu, 4, 8);
    bfexp[3][0] = 0u;

    auto writer = GEBufferWriter::Create();
    writer->setOutputBuffer(inBuf);
    writer->structBegin();
    writer->writeUint4(ops);
    writer->writeUint4(rexp);
    writer->writeUint4(bfexp);
    writer->structEnd();
    writer->sendToBuffer();
    writer->flush();

    auto queue = gte.graphicsEngine->makeCommandQueue(1);
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

    auto reader = GEBufferReader::Create();
    reader->setInputBuffer(outBuf);
    reader->setStructLayout(outLayout);
    reader->structBegin();
    auto highs = FVec<4>::Create();
    auto lows = FVec<4>::Create();
    auto counts = FVec<4>::Create();
    auto revOK = FVec<4>::Create();
    auto bfOK = FVec<4>::Create();
    reader->getFloat4(highs);
    reader->getFloat4(lows);
    reader->getFloat4(counts);
    reader->getFloat4(revOK);
    reader->getFloat4(bfOK);
    reader->structEnd();
    reader->reset();

    for (int i = 0; i < 4; ++i) {
        expectF("firstbithigh", i, highs[i][0], (float)kCases[i].high);
        expectF("firstbitlow", i, lows[i][0], (float)kCases[i].low);
        expectF("countbits", i, counts[i][0], (float)kCases[i].count);
        // 1.0 means the GPU's reversebits matched the host reference for op_i.
        expectF("reversebits", i, revOK[i][0], 1.0f);
    }
    // §5.3-C — extract (unsigned), extract (signed), insert all matched host.
    expectF("bitfieldExtract-u", 0, bfOK[0][0], 1.0f);
    expectF("bitfieldExtract-s", 1, bfOK[1][0], 1.0f);
    expectF("bitfieldInsert", 2, bfOK[2][0], 1.0f);

    OmegaGTE::Close(gte);

    const bool ok = !failFlag();
    std::cout << (ok ? "PASS: bitfield ops" : "FAIL: bitfield ops") << "\n";
    return ok ? 0 : 1;
}
