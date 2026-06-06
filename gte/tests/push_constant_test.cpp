/// Pipeline-Completion §2.2 / OmegaSL §10.2 — push-constant (`constant<T>`)
/// GPU integration test. Backend-independent: uses only the OmegaGTE public
/// API + runtime OmegaSL compilation, so the same source builds and runs on
/// Metal, Vulkan, and D3D12 (mirrors matrix_ops_test.cpp). Headless — a single
/// compute dispatch, then a buffer readback.
///
/// A compute kernel reads a `constant<PushParams>` push constant (set via
/// `setComputeConstants`, with no buffer allocation) and writes derived values
/// into an output buffer the host reads back. This exercises the Phase-B
/// runtime path end-to-end: the command buffer scans the bound pipeline's
/// layout for the single OMEGASL_SHADER_PUSH_CONSTANT_DESC slot and feeds the
/// bytes to the backend's inline-constant mechanism (Metal `setBytes`, D3D12
/// `SetComputeRoot32BitConstants`, Vulkan `vkCmdPushConstants`).
///
/// The push struct is all-16-byte-aligned (two float4s) so the raw bytes the
/// host passes match the shader's natural layout with no padding ambiguity —
/// `setComputeConstants` does no packing, the caller owns the layout.

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GTEMath.h>
#include <omegaGTE/GTEShader.h>
#include "GTETestEntryPoint.h"

#include <cmath>
#include <cstdint>
#include <iostream>

using namespace OmegaGTE;

namespace {

OmegaCommon::String kShaders = R"(

struct PushParams {
    float4 scale;
    float4 bias;
};

struct OutData {
    float4 a;   // scale * 2
    float4 b;   // bias + scale
};

constant<PushParams> pc     : 0;
buffer<OutData>      outBuf : 1;

[in pc, out outBuf]
compute(x=1,y=1,z=1)
void pcKernel(uint3 tid : GlobalThreadID){
    outBuf[0].a = pc.scale * 2.0;
    outBuf[0].b = pc.bias + pc.scale;
}

)";

// Host mirror of `PushParams` — two float4s, 32 bytes, all 16-aligned, so the
// byte image matches the shader's `constant PushParams&` exactly.
struct PushParamsHost {
    float scale[4];
    float bias[4];
};

bool approx(float got, float want, float tol = 1e-3f) {
    return std::fabs(got - want) <= tol;
}

bool &failFlag() { static bool f = false; return f; }

}  // namespace

GTE_TEST_ENTRY_POINT {
    (void)argc;
    (void)argv;
    auto gte = OmegaGTE::InitWithDefaultDevice();

    auto compiled = gte.omegaSlCompiler->compile({OmegaSLCompiler::Source::fromString(kShaders)});
    auto lib = gte.graphicsEngine->loadShaderLibraryRuntime(compiled);

    ComputePipelineDescriptor pd{};
    pd.computeFunc = lib->shaders["pcKernel"];
    if (!pd.computeFunc) {
        std::cerr << "pcKernel shader not found\n";
        OmegaGTE::Close(gte);
        return 1;
    }
    auto pipeline = gte.graphicsEngine->makeComputePipelineState(pd);

    const auto outLayout = OmegaCommon::Vector<omegasl_data_type>{OMEGASL_FLOAT4, OMEGASL_FLOAT4};
    const size_t outSize = omegaSLStructStride(outLayout);
    auto outBuf = gte.graphicsEngine->makeBuffer({BufferDescriptor::Upload, outSize, outSize});

    PushParamsHost params{};
    params.scale[0] = 1; params.scale[1] = 2; params.scale[2] = 3; params.scale[3] = 4;
    params.bias[0] = 10; params.bias[1] = 20; params.bias[2] = 30; params.bias[3] = 40;

    OmegaGTE::GECommandQueueDesc queueDesc{};
    queueDesc.maxBufferCount = 1;
    auto queue = gte.graphicsEngine->makeCommandQueue(queueDesc);
    auto cmd = queue->getAvailableBuffer();
    GEComputePassDescriptor pass{};
    cmd->startComputePass(pass);
    cmd->setComputePipelineState(pipeline);
    cmd->bindResourceAtComputeShader(outBuf, 1);
    // The push constant — no buffer, set inline from the bound pipeline's slot.
    cmd->setComputeConstants(&params, sizeof(params));
    cmd->dispatchThreads(1, 1, 1);
    cmd->finishComputePass();
    queue->submitCommandBuffer(cmd);
    queue->commitToGPUAndWait();

    auto reader = GEBufferReader::Create();
    reader->setInputBuffer(outBuf);
    reader->setStructLayout(outLayout);
    reader->structBegin();
    auto a = FVec<4>::Create();
    auto b = FVec<4>::Create();
    reader->getFloat4(a);
    reader->getFloat4(b);
    reader->structEnd();
    reader->reset();

    for (unsigned i = 0; i < 4; ++i) {
        const float wantA = params.scale[i] * 2.f;
        if (!approx(a[i][0], wantA)) {
            std::cerr << "  FAIL a[" << i << "] = " << a[i][0] << " expected " << wantA << "\n";
            failFlag() = true;
        }
        const float wantB = params.bias[i] + params.scale[i];
        if (!approx(b[i][0], wantB)) {
            std::cerr << "  FAIL b[" << i << "] = " << b[i][0] << " expected " << wantB << "\n";
            failFlag() = true;
        }
    }

    OmegaGTE::Close(gte);

    const bool ok = !failFlag();
    std::cout << (ok ? "PASS: push constants" : "FAIL: push constants") << "\n";
    return ok ? 0 : 1;
}
