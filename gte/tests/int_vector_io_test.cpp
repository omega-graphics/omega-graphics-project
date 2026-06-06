/// OmegaSL §5.3 follow-up — integer / unsigned scalar+vector buffer-IO
/// round-trip. Backend-independent: uses only the OmegaGTE public API + runtime
/// OmegaSL compilation, so the same source builds and runs on Metal, Vulkan,
/// and D3D12 (mirrors matrix_ops_test.cpp / bitfield_ops_test.cpp). Headless —
/// one compute dispatch then a buffer readback.
///
/// This is the readback counterpart to the writer's `writeInt*` / `writeUint*`:
/// it exercises the new `GEBufferReader::getInt*` / `getUint*` scalar+vector
/// API (see GTEShader.h). bitfield_ops_test.cpp previously had to read its
/// integer results back as `float4` precisely because that reader API did not
/// exist yet — this test reads the integers back as integers.
///
/// The struct interleaves sub-16 scalars (`int`, `uint`) ahead of wider vectors
/// so the round-trip only succeeds if the writer/reader insert correct
/// per-member align-then-place padding for the int/uint family (§2.4-1) — the
/// integer analogue of the float `mixedLayout` case in matrix_ops_test.cpp.
/// Each component is doubled by the kernel via component-wise constructors
/// (the conservative op surface used by the other GPU tests), so a misaligned
/// read yields a mismatch rather than a coincidental pass.

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GTEMath.h>
#include <omegaGTE/GTEShader.h>
#include "GTETestEntryPoint.h"

#include <cstdint>
#include <iostream>

using namespace OmegaGTE;

namespace {

OmegaCommon::String kShaders = R"(

struct IntIO {
    int   s;    // @0   align 4
    int2  v2;   // @8   align 8  (4-byte gap after s)
    int3  v3;   // @16  align 16
    int4  v4;   // @32  align 16
    uint  us;   // @48  align 4
    uint2 uv2;  // @56  align 8
    uint3 uv3;  // @64  align 16
    uint4 uv4;  // @80  align 16
};

buffer<IntIO> inBuf  : 0;
buffer<IntIO> outBuf : 1;

[in inBuf, out outBuf]
compute(x=1,y=1,z=1)
void intIO(uint3 tid : GlobalThreadID){
    int   s   = inBuf[0].s;
    int2  v2  = inBuf[0].v2;
    int3  v3  = inBuf[0].v3;
    int4  v4  = inBuf[0].v4;
    uint  us  = inBuf[0].us;
    uint2 uv2 = inBuf[0].uv2;
    uint3 uv3 = inBuf[0].uv3;
    uint4 uv4 = inBuf[0].uv4;

    outBuf[0].s   = s + s;
    outBuf[0].v2  = int2(v2.x + v2.x, v2.y + v2.y);
    outBuf[0].v3  = int3(v3.x + v3.x, v3.y + v3.y, v3.z + v3.z);
    outBuf[0].v4  = int4(v4.x + v4.x, v4.y + v4.y, v4.z + v4.z, v4.w + v4.w);
    outBuf[0].us  = us + us;
    outBuf[0].uv2 = uint2(uv2.x + uv2.x, uv2.y + uv2.y);
    outBuf[0].uv3 = uint3(uv3.x + uv3.x, uv3.y + uv3.y, uv3.z + uv3.z);
    outBuf[0].uv4 = uint4(uv4.x + uv4.x, uv4.y + uv4.y, uv4.z + uv4.z, uv4.w + uv4.w);
}

)";

// Distinct values per field/component so a wrong offset surfaces as a mismatch.
constexpr int kS = 7;
constexpr int kV2[2] = {11, -12};
constexpr int kV3[3] = {21, -22, 23};
constexpr int kV4[4] = {31, -32, 33, -34};
constexpr unsigned kUS = 41u;
constexpr unsigned kUV2[2] = {51u, 52u};
constexpr unsigned kUV3[3] = {61u, 62u, 63u};
constexpr unsigned kUV4[4] = {71u, 72u, 73u, 74u};

bool &failFlag() { static bool f = false; return f; }

void expectI(const char *name, unsigned idx, int got, int want) {
    if (got != want) {
        std::cerr << "  FAIL " << name << "[" << idx << "] = " << got
                  << " expected " << want << "\n";
        failFlag() = true;
    }
}
void expectU(const char *name, unsigned idx, unsigned got, unsigned want) {
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
    pd.computeFunc = lib->shaders["intIO"];
    if (!pd.computeFunc) {
        std::cerr << "intIO shader not found\n";
        OmegaGTE::Close(gte);
        return 1;
    }
    auto pipeline = gte.graphicsEngine->makeComputePipelineState(pd);

    const auto layout = OmegaCommon::Vector<omegasl_data_type>{
        OMEGASL_INT, OMEGASL_INT2, OMEGASL_INT3, OMEGASL_INT4,
        OMEGASL_UINT, OMEGASL_UINT2, OMEGASL_UINT3, OMEGASL_UINT4};
    const size_t structSize = omegaSLStructStride(layout);

    auto inBuf = gte.graphicsEngine->makeBuffer({BufferDescriptor::Upload, structSize, structSize});
    auto outBuf = gte.graphicsEngine->makeBuffer({BufferDescriptor::Upload, structSize, structSize});

    // Host-side inputs.
    int s = kS;
    auto v2 = IVec<2>::Create();
    for (unsigned i = 0; i < 2; ++i) v2[i][0] = kV2[i];
    auto v3 = IVec<3>::Create();
    for (unsigned i = 0; i < 3; ++i) v3[i][0] = kV3[i];
    auto v4 = IVec<4>::Create();
    for (unsigned i = 0; i < 4; ++i) v4[i][0] = kV4[i];
    unsigned us = kUS;
    auto uv2 = UVec<2>::Create();
    for (unsigned i = 0; i < 2; ++i) uv2[i][0] = kUV2[i];
    auto uv3 = UVec<3>::Create();
    for (unsigned i = 0; i < 3; ++i) uv3[i][0] = kUV3[i];
    auto uv4 = UVec<4>::Create();
    for (unsigned i = 0; i < 4; ++i) uv4[i][0] = kUV4[i];

    auto writer = GEBufferWriter::Create();
    writer->setOutputBuffer(inBuf);
    writer->structBegin();
    writer->writeInt(s);
    writer->writeInt2(v2);
    writer->writeInt3(v3);
    writer->writeInt4(v4);
    writer->writeUint(us);
    writer->writeUint2(uv2);
    writer->writeUint3(uv3);
    writer->writeUint4(uv4);
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
    reader->setStructLayout(layout);
    reader->structBegin();
    int oS = 0;
    auto oV2 = IVec<2>::Create();
    auto oV3 = IVec<3>::Create();
    auto oV4 = IVec<4>::Create();
    unsigned oUS = 0;
    auto oUV2 = UVec<2>::Create();
    auto oUV3 = UVec<3>::Create();
    auto oUV4 = UVec<4>::Create();
    reader->getInt(oS);
    reader->getInt2(oV2);
    reader->getInt3(oV3);
    reader->getInt4(oV4);
    reader->getUint(oUS);
    reader->getUint2(oUV2);
    reader->getUint3(oUV3);
    reader->getUint4(oUV4);
    reader->structEnd();
    reader->reset();

    // Each component is doubled by the kernel.
    expectI("s", 0, oS, kS * 2);
    for (unsigned i = 0; i < 2; ++i) expectI("v2", i, oV2[i][0], kV2[i] * 2);
    for (unsigned i = 0; i < 3; ++i) expectI("v3", i, oV3[i][0], kV3[i] * 2);
    for (unsigned i = 0; i < 4; ++i) expectI("v4", i, oV4[i][0], kV4[i] * 2);
    expectU("us", 0, oUS, kUS * 2u);
    for (unsigned i = 0; i < 2; ++i) expectU("uv2", i, oUV2[i][0], kUV2[i] * 2u);
    for (unsigned i = 0; i < 3; ++i) expectU("uv3", i, oUV3[i][0], kUV3[i] * 2u);
    for (unsigned i = 0; i < 4; ++i) expectU("uv4", i, oUV4[i][0], kUV4[i] * 2u);

    OmegaGTE::Close(gte);

    const bool ok = !failFlag();
    std::cout << (ok ? "PASS: int vector IO" : "FAIL: int vector IO") << "\n";
    return ok ? 0 : 1;
}
