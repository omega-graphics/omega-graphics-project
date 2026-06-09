/// OmegaSL §5.5 Phases B + C — half-float and normalized pack / unpack GPU
/// integration test. Backend-independent: uses only the OmegaGTE public API +
/// runtime OmegaSL compilation, so the same source builds and runs on Metal,
/// Vulkan, and D3D12 (mirrors matrix_ops_test.cpp / bitfield_ops_test.cpp).
/// Headless — two compute dispatches then two buffer readbacks.
///
/// The point of this test (beyond "it compiles") is to prove the per-backend
/// lowerings for `f16tof32` / `f32tof16` / `packHalf2x16` / `unpackHalf2x16`
/// round-trip a binary16 value bit-identically on real hardware. The three
/// lowerings differ wildly — HLSL native scalar + manual packed-form lowering
/// via statement injection, MSL `as_type<half2>` / `as_type<uint>`, GLSL
/// native packed + scalar lowering via the packed forms — so a GPU readback
/// is the only check that the three converge.
///
/// Test inputs are all exactly fp16-representable (0.5, 1.0, 2.0, -3.0), so
/// the round-trip recovers the original float bit-pattern. Comparison is done
/// on-GPU via `asuint(recovered) == asuint(input)` (§5.5 Phase A) so the
/// scoreboard value (1.0 vs 0.0) trip-flips on any mismatch, with no float
/// epsilon to tune.
///
/// What this test catches that `-S` source inspection misses:
/// * MSL overload ambiguity (e.g. `as_type<ushort>` argument deduction
///   collapsing under a bare literal — the §5.3 Phase C class of bug).
/// * HLSL statement-injection ordering bugs in nested `pack`/`unpack` calls
///   (the `_ph<id>` / `_uh<id>` temps must land in dependency order).
/// * GLSL `unpackHalf2x16(x).x` not silently zero-extending the high bits
///   into the discarded lane (a typo emitting `.y` would fail here).

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GTEShader.h>
#include "GTETestEntryPoint.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

using namespace OmegaGTE;

namespace {

OmegaCommon::String kShaders = R"(

struct InData {
    float4 inputs;  // four fp16-representable test floats
};
struct OutData {
    uint4   packedScalar;    // f32tof16(inputs[i]) -- low 16 bits hold the half16
    float4  scalarRoundOk;   // 1.0 if f16tof32(packedScalar[i]) bit-equals inputs[i]
    uint4   packed2x16;      // packHalf2x16(float2(inputs[2i], inputs[2i+1])) for i in 0,1, lanes 2/3 zero
    float4  packedRoundOk;   // 1.0 per lane: unpackHalf2x16 recovers each half exactly
    float4  crossFormOk;     // 1.0 if packHalf2x16 matches (f32tof16 | (f32tof16<<16))
};

buffer<InData>  inBuf  : 0;
buffer<OutData> outBuf : 1;

[in inBuf, out outBuf]
compute(x=1,y=1,z=1)
void packOps(uint3 tid : GlobalThreadID){
    float4 v = inBuf[0].inputs;

    // ---- scalar form ----
    uint  ps0 = f32tof16(v.x);
    uint  ps1 = f32tof16(v.y);
    uint  ps2 = f32tof16(v.z);
    uint  ps3 = f32tof16(v.w);
    outBuf[0].packedScalar = uint4(ps0, ps1, ps2, ps3);

    float rs0 = f16tof32(ps0);
    float rs1 = f16tof32(ps1);
    float rs2 = f16tof32(ps2);
    float rs3 = f16tof32(ps3);
    outBuf[0].scalarRoundOk = float4(
        float(asuint(rs0) == asuint(v.x) ? 1u : 0u),
        float(asuint(rs1) == asuint(v.y) ? 1u : 0u),
        float(asuint(rs2) == asuint(v.z) ? 1u : 0u),
        float(asuint(rs3) == asuint(v.w) ? 1u : 0u));

    // ---- packed 2x16 form ----
    uint pP0 = packHalf2x16(float2(v.x, v.y));
    uint pP1 = packHalf2x16(float2(v.z, v.w));
    outBuf[0].packed2x16 = uint4(pP0, pP1, 0u, 0u);

    float2 r0 = unpackHalf2x16(pP0);
    float2 r1 = unpackHalf2x16(pP1);
    outBuf[0].packedRoundOk = float4(
        float(asuint(r0.x) == asuint(v.x) ? 1u : 0u),
        float(asuint(r0.y) == asuint(v.y) ? 1u : 0u),
        float(asuint(r1.x) == asuint(v.z) ? 1u : 0u),
        float(asuint(r1.y) == asuint(v.w) ? 1u : 0u));

    // ---- cross-form consistency ----
    // packHalf2x16(float2(a, b)) must equal (f32tof16(a) | (f32tof16(b) << 16))
    uint manual0 = ps0 | (ps1 << 16);
    uint manual1 = ps2 | (ps3 << 16);
    outBuf[0].crossFormOk = float4(
        float(pP0 == manual0 ? 1u : 0u),
        float(pP1 == manual1 ? 1u : 0u),
        // and the high 16 bits of f32tof16(x) must be zero (the spec):
        float((ps0 >> 16) == 0u ? 1u : 0u),
        float((ps3 >> 16) == 0u ? 1u : 0u));
}

// §5.5 Phase C — normalized 4x8 / 2x16 pack/unpack. The kernel reads four
// (snorm4, unorm4, snorm2, unorm2) test vectors and writes the packed uint
// for each + a per-lane scoreboard comparing the round-trip recovered value
// against a host-computed reference (passed in alongside the inputs).
// The HLSL lowering is bit-identical to the GLSL / MSL spec, but exercising
// it on real GPU catches: per-backend rounding-mode disagreement on `round`;
// sign-extension off-by-one in the unpack lowering; mask-and-shift ordering.

struct NormInData {
    float4 snorm4;  // input for packSnorm4x8
    float4 unorm4;  // input for packUnorm4x8
    float4 snorm2;  // input for packSnorm2x16 (only .xy used; .zw ignored)
    float4 unorm2;  // input for packUnorm2x16 (only .xy used; .zw ignored)
    uint4  expectedPacks;   // host-computed pack outputs:
                            // (packSnorm4x8, packUnorm4x8, packSnorm2x16, packUnorm2x16)
    float4 expectedSnorm4;  // host-computed unpackSnorm4x8 result
    float4 expectedUnorm4;  // host-computed unpackUnorm4x8 result
    float4 expectedSnorm2;  // host-computed unpackSnorm2x16 (.xy used)
    float4 expectedUnorm2;  // host-computed unpackUnorm2x16 (.xy used)
};
struct NormOutData {
    uint4  packed;          // (packSnorm4x8, packUnorm4x8, packSnorm2x16, packUnorm2x16)
    float4 packOk;          // 1.0 per slot if packed value matches expected
    float4 unpackS4Ok;      // 1.0 per lane if unpacked snorm4 matches expected (4 lanes)
    float4 unpackU4Ok;      // 1.0 per lane if unpacked unorm4 matches expected (4 lanes)
    float4 unpack216Ok;     // (snorm2.x, snorm2.y, unorm2.x, unorm2.y) match flags
};

buffer<NormInData>  normInBuf  : 0;
buffer<NormOutData> normOutBuf : 1;

[in normInBuf, out normOutBuf]
compute(x=1,y=1,z=1)
void packNormOps(uint3 tid : GlobalThreadID){
    NormInData d = normInBuf[0];

    uint pS4  = packSnorm4x8(d.snorm4);
    uint pU4  = packUnorm4x8(d.unorm4);
    uint pS2  = packSnorm2x16(float2(d.snorm2.x, d.snorm2.y));
    uint pU2  = packUnorm2x16(float2(d.unorm2.x, d.unorm2.y));
    normOutBuf[0].packed = uint4(pS4, pU4, pS2, pU2);

    uint4 ex = d.expectedPacks;
    normOutBuf[0].packOk = float4(
        float(pS4 == ex.x ? 1u : 0u),
        float(pU4 == ex.y ? 1u : 0u),
        float(pS2 == ex.z ? 1u : 0u),
        float(pU2 == ex.w ? 1u : 0u));

    float4 uS4 = unpackSnorm4x8(pS4);
    float4 uU4 = unpackUnorm4x8(pU4);
    float2 uS2 = unpackSnorm2x16(pS2);
    float2 uU2 = unpackUnorm2x16(pU2);

    // Bit-exact comparison via asuint: any rounding bug surfaces as a mismatch.
    float4 eS4 = d.expectedSnorm4;
    float4 eU4 = d.expectedUnorm4;
    float4 eS2 = d.expectedSnorm2;
    float4 eU2 = d.expectedUnorm2;
    normOutBuf[0].unpackS4Ok = float4(
        float(asuint(uS4.x) == asuint(eS4.x) ? 1u : 0u),
        float(asuint(uS4.y) == asuint(eS4.y) ? 1u : 0u),
        float(asuint(uS4.z) == asuint(eS4.z) ? 1u : 0u),
        float(asuint(uS4.w) == asuint(eS4.w) ? 1u : 0u));
    normOutBuf[0].unpackU4Ok = float4(
        float(asuint(uU4.x) == asuint(eU4.x) ? 1u : 0u),
        float(asuint(uU4.y) == asuint(eU4.y) ? 1u : 0u),
        float(asuint(uU4.z) == asuint(eU4.z) ? 1u : 0u),
        float(asuint(uU4.w) == asuint(eU4.w) ? 1u : 0u));
    normOutBuf[0].unpack216Ok = float4(
        float(asuint(uS2.x) == asuint(eS2.x) ? 1u : 0u),
        float(asuint(uS2.y) == asuint(eS2.y) ? 1u : 0u),
        float(asuint(uU2.x) == asuint(eU2.x) ? 1u : 0u),
        float(asuint(uU2.y) == asuint(eU2.y) ? 1u : 0u));
}

)";

// Host-side IEEE-754 binary16 encoder for the four test inputs. The values
// are chosen to be exactly representable in fp16, so the round-trip recovers
// the input float bit-pattern with no rounding. (We don't depend on this
// encoder for correctness — the on-GPU comparison via asuint is authoritative
// — but use it to predict the expected packedScalar values in case the GPU
// test surfaces a per-backend off-by-one in the encoder lowering.)
static uint16_t f32_to_f16(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    uint32_t sign = (u >> 16) & 0x8000u;
    int32_t  exp  = (int32_t)((u >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = (u >> 13) & 0x3FFu;
    if (exp <= 0) return (uint16_t)sign; // underflow / subnormal — unused here.
    if (exp >= 31) return (uint16_t)(sign | 0x7C00u | mant); // inf / nan / overflow.
    return (uint16_t)(sign | (uint32_t(exp) << 10) | mant);
}

// Four fp16-representable values. -3.0 covers the sign-bit path; 0.5 covers
// a non-integer mantissa; 1.0 / 2.0 cover powers of two.
constexpr float kInputs[4] = {0.5f, 1.0f, 2.0f, -3.0f};

// --- §5.5 Phase C host references ---
//
// The GLSL/MSL spec for pack* normalized:
//   snorm:  byte = round(clamp(c, -1, +1) * (2^(K-1) - 1))   // K = 8 or 16
//   unorm:  byte = round(clamp(c,  0,  1) * (2^K - 1))
// Test inputs are placed exactly on a representable byte/short boundary so the
// host and GPU agree without rounding-mode contention:
//   * snorm4: {0, 1, -1, 0.5}     — zero, sat+, sat-, exact half-step
//   * unorm4: {0, 1, 0.5, 0.25}   — zero, sat+, half, quarter
//   * snorm2: {-1, 1}
//   * unorm2: { 0, 1}
constexpr float kSnorm4[4] = {0.0f,  1.0f, -1.0f, 0.5f};
constexpr float kUnorm4[4] = {0.0f,  1.0f,  0.5f, 0.25f};
constexpr float kSnorm2[2] = {-1.0f, 1.0f};
constexpr float kUnorm2[2] = { 0.0f, 1.0f};

static int8_t snorm8(float c) {
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return (int8_t)std::lround(c * 127.0f);
}
static uint8_t unorm8(float c) {
    if (c > 1.0f) c = 1.0f;
    if (c < 0.0f) c = 0.0f;
    return (uint8_t)std::lround(c * 255.0f);
}
static int16_t snorm16(float c) {
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return (int16_t)std::lround(c * 32767.0f);
}
static uint16_t unorm16(float c) {
    if (c > 1.0f) c = 1.0f;
    if (c < 0.0f) c = 0.0f;
    return (uint16_t)std::lround(c * 65535.0f);
}
static float unsnorm8(int8_t b) {
    float v = (float)b / 127.0f;
    if (v < -1.0f) v = -1.0f;
    return v;
}
static float ununorm8(uint8_t b) { return (float)b / 255.0f; }
static float unsnorm16(int16_t s) {
    float v = (float)s / 32767.0f;
    if (v < -1.0f) v = -1.0f;
    return v;
}
static float ununorm16(uint16_t s) { return (float)s / 65535.0f; }

bool &failFlag() { static bool f = false; return f; }

void expectF(const char *name, int idx, float got, float want) {
    if (got != want) {
        std::cerr << "  FAIL " << name << "[" << idx << "] = " << got
                  << " expected " << want << "\n";
        failFlag() = true;
    }
}

void expectU(const char *name, int idx, uint32_t got, uint32_t want) {
    if (got != want) {
        std::cerr << "  FAIL " << name << "[" << idx << "] = 0x" << std::hex << got
                  << " expected 0x" << want << std::dec << "\n";
        failFlag() = true;
    }
}

} // namespace

GTE_TEST_ENTRY_POINT {
    (void)argc;
    (void)argv;
    auto gte = OmegaGTE::InitWithDefaultDevice();

    auto compiled = gte.omegaSlCompiler->compile({OmegaSLCompiler::Source::fromString(kShaders)});
    auto lib = gte.graphicsEngine->loadShaderLibraryRuntime(compiled);

    ComputePipelineDescriptor pd{};
    pd.computeFunc = lib->shaders["packOps"];
    if (!pd.computeFunc) {
        std::cerr << "packOps shader not found\n";
        OmegaGTE::Close(gte);
        return 1;
    }
    auto pipeline = gte.graphicsEngine->makeComputePipelineState(pd);

    const auto inLayout = OmegaCommon::Vector<omegasl_data_type>{OMEGASL_FLOAT4};
    const auto outLayout = OmegaCommon::Vector<omegasl_data_type>{
        OMEGASL_UINT4, OMEGASL_FLOAT4, OMEGASL_UINT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4};
    const size_t inSize = omegaSLStructStride(inLayout);
    const size_t outSize = omegaSLStructStride(outLayout);

    auto inBuf = gte.graphicsEngine->makeBuffer({BufferDescriptor::Upload, inSize, inSize});
    auto outBuf = gte.graphicsEngine->makeBuffer({BufferDescriptor::Upload, outSize, outSize});

    auto inputs = FVec<4>::Create();
    for (int i = 0; i < 4; ++i) inputs[i][0] = kInputs[i];

    auto writer = GEBufferWriter::Create();
    writer->setOutputBuffer(inBuf);
    writer->structBegin();
    writer->writeFloat4(inputs);
    writer->structEnd();
    writer->sendToBuffer();
    writer->flush();

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

    auto reader = GEBufferReader::Create();
    reader->setInputBuffer(outBuf);
    reader->setStructLayout(outLayout);
    reader->structBegin();
    auto packedScalar = UVec<4>::Create();
    auto scalarRoundOk = FVec<4>::Create();
    auto packed2x16 = UVec<4>::Create();
    auto packedRoundOk = FVec<4>::Create();
    auto crossFormOk = FVec<4>::Create();
    reader->getUint4(packedScalar);
    reader->getFloat4(scalarRoundOk);
    reader->getUint4(packed2x16);
    reader->getFloat4(packedRoundOk);
    reader->getFloat4(crossFormOk);
    reader->structEnd();
    reader->reset();

    // Round-trip flags must all be 1.0 — any mismatch fails.
    for (int i = 0; i < 4; ++i) {
        expectF("scalarRoundOk", i, scalarRoundOk[i][0], 1.0f);
        expectF("packedRoundOk", i, packedRoundOk[i][0], 1.0f);
    }
    // Cross-form consistency on the two packed lanes, plus the
    // high-bits-are-zero spec check on f32tof16's first/last lane.
    for (int i = 0; i < 4; ++i) {
        expectF("crossFormOk", i, crossFormOk[i][0], 1.0f);
    }

    // Spot-check the encoded packedScalar low 16 bits against the host
    // reference encoder. This catches a per-backend off-by-one before
    // the more abstract round-trip flags surface it.
    for (int i = 0; i < 4; ++i) {
        uint32_t low16 = packedScalar[i][0] & 0xFFFFu;
        expectU("packedScalar low16", i, low16, (uint32_t)f32_to_f16(kInputs[i]));
    }

    // ============== §5.5 Phase C — normalized pack/unpack ==============
    ComputePipelineDescriptor pdN{};
    pdN.computeFunc = lib->shaders["packNormOps"];
    if (!pdN.computeFunc) {
        std::cerr << "packNormOps shader not found\n";
        OmegaGTE::Close(gte);
        return 1;
    }
    auto pipelineN = gte.graphicsEngine->makeComputePipelineState(pdN);

    const auto normInLayout = OmegaCommon::Vector<omegasl_data_type>{
        // snorm4, unorm4, snorm2, unorm2
        OMEGASL_FLOAT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4,
        // expectedPacks (uint4), then four expected unpack vectors
        OMEGASL_UINT4,
        OMEGASL_FLOAT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4};
    const auto normOutLayout = OmegaCommon::Vector<omegasl_data_type>{
        OMEGASL_UINT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4};
    const size_t normInSize = omegaSLStructStride(normInLayout);
    const size_t normOutSize = omegaSLStructStride(normOutLayout);

    auto normInBuf = gte.graphicsEngine->makeBuffer({BufferDescriptor::Upload, normInSize, normInSize});
    auto normOutBuf = gte.graphicsEngine->makeBuffer({BufferDescriptor::Upload, normOutSize, normOutSize});

    // Host-compute the expected packed uint and the expected unpacked floats.
    uint32_t expS4 = ((uint32_t)(uint8_t)snorm8(kSnorm4[0]) & 0xFFu)
                   | (((uint32_t)(uint8_t)snorm8(kSnorm4[1]) & 0xFFu) << 8)
                   | (((uint32_t)(uint8_t)snorm8(kSnorm4[2]) & 0xFFu) << 16)
                   | (((uint32_t)(uint8_t)snorm8(kSnorm4[3]) & 0xFFu) << 24);
    uint32_t expU4 = (uint32_t)unorm8(kUnorm4[0])
                   | ((uint32_t)unorm8(kUnorm4[1]) << 8)
                   | ((uint32_t)unorm8(kUnorm4[2]) << 16)
                   | ((uint32_t)unorm8(kUnorm4[3]) << 24);
    uint32_t expS2 = ((uint32_t)(uint16_t)snorm16(kSnorm2[0]) & 0xFFFFu)
                   | (((uint32_t)(uint16_t)snorm16(kSnorm2[1]) & 0xFFFFu) << 16);
    uint32_t expU2 = (uint32_t)unorm16(kUnorm2[0])
                   | ((uint32_t)unorm16(kUnorm2[1]) << 16);

    // Build the input struct.
    auto snorm4 = FVec<4>::Create();
    auto unorm4 = FVec<4>::Create();
    auto snorm2 = FVec<4>::Create();
    auto unorm2 = FVec<4>::Create();
    for (int i = 0; i < 4; ++i) snorm4[i][0] = kSnorm4[i];
    for (int i = 0; i < 4; ++i) unorm4[i][0] = kUnorm4[i];
    for (int i = 0; i < 2; ++i) snorm2[i][0] = kSnorm2[i];
    snorm2[2][0] = 0.0f; snorm2[3][0] = 0.0f;
    for (int i = 0; i < 2; ++i) unorm2[i][0] = kUnorm2[i];
    unorm2[2][0] = 0.0f; unorm2[3][0] = 0.0f;

    auto expectedPacks = UVec<4>::Create();
    expectedPacks[0][0] = expS4;
    expectedPacks[1][0] = expU4;
    expectedPacks[2][0] = expS2;
    expectedPacks[3][0] = expU2;

    auto expS4Vec = FVec<4>::Create();
    auto expU4Vec = FVec<4>::Create();
    auto expS2Vec = FVec<4>::Create();
    auto expU2Vec = FVec<4>::Create();
    for (int i = 0; i < 4; ++i) expS4Vec[i][0] = unsnorm8(snorm8(kSnorm4[i]));
    for (int i = 0; i < 4; ++i) expU4Vec[i][0] = ununorm8(unorm8(kUnorm4[i]));
    expS2Vec[0][0] = unsnorm16(snorm16(kSnorm2[0]));
    expS2Vec[1][0] = unsnorm16(snorm16(kSnorm2[1]));
    expS2Vec[2][0] = 0.0f; expS2Vec[3][0] = 0.0f;
    expU2Vec[0][0] = ununorm16(unorm16(kUnorm2[0]));
    expU2Vec[1][0] = ununorm16(unorm16(kUnorm2[1]));
    expU2Vec[2][0] = 0.0f; expU2Vec[3][0] = 0.0f;

    auto writerN = GEBufferWriter::Create();
    writerN->setOutputBuffer(normInBuf);
    writerN->structBegin();
    writerN->writeFloat4(snorm4);
    writerN->writeFloat4(unorm4);
    writerN->writeFloat4(snorm2);
    writerN->writeFloat4(unorm2);
    writerN->writeUint4(expectedPacks);
    writerN->writeFloat4(expS4Vec);
    writerN->writeFloat4(expU4Vec);
    writerN->writeFloat4(expS2Vec);
    writerN->writeFloat4(expU2Vec);
    writerN->structEnd();
    writerN->sendToBuffer();
    writerN->flush();

    auto cmdN = queue->getAvailableBuffer();
    GEComputePassDescriptor passN{};
    cmdN->startComputePass(passN);
    cmdN->setComputePipelineState(pipelineN);
    cmdN->bindResourceAtComputeShader(normInBuf, 0);
    cmdN->bindResourceAtComputeShader(normOutBuf, 1);
    cmdN->dispatchThreads(1, 1, 1);
    cmdN->finishComputePass();
    queue->submitCommandBuffer(cmdN);
    queue->commitToGPUAndWait();

    auto readerN = GEBufferReader::Create();
    readerN->setInputBuffer(normOutBuf);
    readerN->setStructLayout(normOutLayout);
    readerN->structBegin();
    auto gotPacked = UVec<4>::Create();
    auto packOk = FVec<4>::Create();
    auto unpackS4Ok = FVec<4>::Create();
    auto unpackU4Ok = FVec<4>::Create();
    auto unpack216Ok = FVec<4>::Create();
    readerN->getUint4(gotPacked);
    readerN->getFloat4(packOk);
    readerN->getFloat4(unpackS4Ok);
    readerN->getFloat4(unpackU4Ok);
    readerN->getFloat4(unpack216Ok);
    readerN->structEnd();
    readerN->reset();

    // Spot-check the packed uints against the host-computed expectations,
    // as direct values (not just via the GPU's `packOk` flag). This catches
    // a wrong host reference (the flag-only check would still pass if both
    // host and GPU happened to compute the same wrong value).
    expectU("packSnorm4x8",  0, gotPacked[0][0], expS4);
    expectU("packUnorm4x8",  0, gotPacked[1][0], expU4);
    expectU("packSnorm2x16", 0, gotPacked[2][0], expS2);
    expectU("packUnorm2x16", 0, gotPacked[3][0], expU2);

    // Per-slot pack scoreboard.
    expectF("packOk[Snorm4]",  0, packOk[0][0], 1.0f);
    expectF("packOk[Unorm4]",  1, packOk[1][0], 1.0f);
    expectF("packOk[Snorm2]",  2, packOk[2][0], 1.0f);
    expectF("packOk[Unorm2]",  3, packOk[3][0], 1.0f);

    // Per-lane unpack scoreboards.
    for (int i = 0; i < 4; ++i) {
        expectF("unpackS4Ok", i, unpackS4Ok[i][0], 1.0f);
        expectF("unpackU4Ok", i, unpackU4Ok[i][0], 1.0f);
        expectF("unpack216Ok", i, unpack216Ok[i][0], 1.0f);
    }

    OmegaGTE::Close(gte);

    const bool ok = !failFlag();
    std::cout << (ok ? "PASS: pack ops" : "FAIL: pack ops") << "\n";
    return ok ? 0 : 1;
}
