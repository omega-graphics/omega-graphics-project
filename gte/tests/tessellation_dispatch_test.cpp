/// OmegaSL §16 Phase E — tessellation hull-dispatch GPU integration test.
///
/// On Metal a `hull` stage lowers to a compute kernel dispatched **one thread
/// per patch** (§16-E4a per-patch codegen): the thread loops over the patch's
/// control points, stores each to the per-control-point output buffer, and
/// writes the patch's tessellation factors (via the patch-constant function)
/// into a `MTL{Triangle,Quad}TessellationFactorsHalf` buffer. This test drives
/// that dispatch directly and reads BOTH outputs back:
///
///   * the per-control-point output buffer — proves the dispatch ran and the
///     per-patch control-point loop indexed correctly (`hullOut[vid] ==
///     controlPoints[vid]` for every control point of the patch);
///   * the tessellation-factor buffer — proves the patch-constant function ran
///     and its factors landed. `GEBuffer` exposes no raw byte view and the
///     factors are `half`, so we read the buffer back as `uint`s and assert the
///     IEEE-754 half BIT PATTERNS: 1.0 == 0x3C00 (the triangle patchfn) and
///     2.0 == 0x4000 (the quad patchfn).
///
/// This is a Metal-specific verification because ONLY Metal models tessellation
/// as a separate compute dispatch with an intermediate factor buffer. On D3D12
/// a hull is an `hs_6_0` stage and on Vulkan a `.tesc`; there tessellation runs
/// inside the draw pipeline, so the equivalent verification is a `drawPatches`
/// smoke test (§16 Phase E, D3D12/Vulkan pass). Hence this test is registered
/// only under the Metal backend today.

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GTEMath.h>
#include <omegaGTE/GTEShader.h>
#include "GTETestEntryPoint.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace OmegaGTE;

namespace {

OmegaCommon::String kShaders = R"(

struct ControlPointIn { float4 pos; };
struct ControlPointOut { float4 pos; };

struct DomainOut internal { float4 pos : Position; };

struct TriPatchConstants internal {
    float edges[3]  : TessFactor;
    float inside[1] : InsideTessFactor;
};
struct QuadPatchConstants internal {
    float edges[4]  : TessFactor;
    float inside[2] : InsideTessFactor;
};

buffer<ControlPointIn> controlPoints : 0;
buffer<ControlPointOut> hullOut : 1;

TriPatchConstants triConstants() {
    TriPatchConstants p;
    p.edges[0] = 1.0; p.edges[1] = 1.0; p.edges[2] = 1.0;
    p.inside[0] = 1.0;
    return p;
}

[in controlPoints, out hullOut]
hull(domain=tri, partitioning=integer, outputtopology=triangle_cw, outputcontrolpoints=3, patchfn=triConstants)
ControlPointOut triHull(uint vid : VertexID) {
    ControlPointOut o;
    o.pos = controlPoints[vid].pos;
    return o;
}

[in controlPoints]
domain(domain=tri)
DomainOut triDomain(float3 loc : DomainLocation) {
    DomainOut o;
    o.pos = controlPoints[0].pos * loc.x
          + controlPoints[1].pos * loc.y
          + controlPoints[2].pos * loc.z;
    return o;
}

QuadPatchConstants quadConstants() {
    QuadPatchConstants p;
    p.edges[0] = 2.0; p.edges[1] = 2.0; p.edges[2] = 2.0; p.edges[3] = 2.0;
    p.inside[0] = 2.0; p.inside[1] = 2.0;
    return p;
}

[in controlPoints, out hullOut]
hull(domain=quad, partitioning=fractional_even, outputtopology=triangle_ccw, outputcontrolpoints=4, patchfn=quadConstants)
ControlPointOut quadHull(uint vid : VertexID) {
    ControlPointOut o;
    o.pos = controlPoints[vid].pos;
    return o;
}

[in controlPoints]
domain(domain=quad)
DomainOut quadDomain(float2 loc : DomainLocation) {
    DomainOut o;
    o.pos = controlPoints[0].pos * loc.x
          + controlPoints[1].pos * loc.y;
    return o;
}

)";

bool &failFlag() { static bool f = false; return f; }

void expectHalfBits(const char *what, unsigned got, unsigned want) {
    // Compare only the low 16 bits (one packed half).
    if ((got & 0xFFFFu) != (want & 0xFFFFu)) {
        std::cerr << "  FAIL " << what << " = 0x" << std::hex << (got & 0xFFFFu)
                  << " expected 0x" << (want & 0xFFFFu) << std::dec << "\n";
        failFlag() = true;
    }
}

// Dispatch one hull entry over a single patch and verify (a) the per-control-
// point output echoes the input control points and (b) the factor buffer holds
// the expected half bits. `factorHalfBits` is the IEEE-754 half of the patchfn
// factor value (0x3C00 for 1.0, 0x4000 for 2.0). `factorWordCount` is the size
// of the MTL*TessellationFactorsHalf struct in 32-bit words (tri: 8 bytes = 2;
// quad: 12 bytes = 3).
void runHull(GTE &gte, SharedHandle<GTEShaderLibrary> &lib, SharedHandle<GECommandQueue> &queue,
             const char *entry, unsigned controlPointCount,
             unsigned factorWordCount, unsigned factorHalfBits) {
    std::cout << "=== " << entry << " (" << controlPointCount << " control points) ===\n";

    ComputePipelineDescriptor pd{};
    pd.computeFunc = lib->shaders[entry];
    if (!pd.computeFunc) {
        std::cerr << "  FAIL: hull shader '" << entry << "' not found in library\n";
        failFlag() = true;
        return;
    }
    auto pipeline = gte.graphicsEngine->makeComputePipelineState(pd);

    // A control point is a single float4 (16-byte std430 stride).
    const auto cpLayout = OmegaCommon::Vector<omegasl_data_type>{OMEGASL_FLOAT4};
    const size_t cpStride = omegaSLStructStride(cpLayout);

    BufferDescriptor cpDesc{};
    cpDesc.usage = BufferDescriptor::Upload;
    cpDesc.len = cpStride * controlPointCount;
    cpDesc.objectStride = cpStride;
    auto controlPoints = gte.graphicsEngine->makeBuffer(cpDesc);

    BufferDescriptor outDesc{};
    outDesc.usage = BufferDescriptor::Readback;
    outDesc.len = cpStride * controlPointCount;
    outDesc.objectStride = cpStride;
    auto hullOut = gte.graphicsEngine->makeBuffer(outDesc);

    // The tessellation-factor buffer is a raw MTL*TessellationFactorsHalf slab.
    // It is bound at the synthesized buffer slot (== control-point + output
    // buffer count == 2), which is not in the shader's resource layout; the
    // runtime maps an unknown resource id straight to that Metal buffer index.
    BufferDescriptor factorDesc{};
    factorDesc.usage = BufferDescriptor::Readback;
    factorDesc.len = factorWordCount * sizeof(uint32_t);
    factorDesc.objectStride = factorDesc.len;
    auto factors = gte.graphicsEngine->makeBuffer(factorDesc);

    // Seed distinct control-point positions so a mis-indexed copy is visible.
    std::vector<std::array<float, 4>> cpValues(controlPointCount);
    for (unsigned i = 0; i < controlPointCount; ++i) {
        cpValues[i] = {float(i) + 1.f, float(i) + 2.f, float(i) + 3.f, float(i) + 4.f};
    }

    auto writer = GEBufferWriter::Create();
    writer->setOutputBuffer(controlPoints);
    for (unsigned i = 0; i < controlPointCount; ++i) {
        auto v = FVec<4>::Create();
        for (unsigned k = 0; k < 4; ++k) v[k][0] = cpValues[i][k];
        writer->structBegin();
        writer->writeFloat4(v);
        writer->structEnd();
        // One sendToBuffer per struct: it stages the current struct at the
        // running cursor and advances it. structBegin clears the staged blocks,
        // so batching a single sendToBuffer after the loop would write only the
        // last control point.
        writer->sendToBuffer();
    }
    writer->flush();

    // Dispatch one thread per patch — a single patch here.
    auto cmd = queue->getAvailableBuffer();
    GEComputePassDescriptor pass{};
    cmd->startComputePass(pass);
    cmd->setComputePipelineState(pipeline);
    cmd->bindResourceAtComputeShader(controlPoints, 0);
    cmd->bindResourceAtComputeShader(hullOut, 1);
    cmd->bindResourceAtComputeShader(factors, 2);
    cmd->dispatchThreads(1, 1, 1);
    cmd->finishComputePass();
    queue->submitCommandBuffer(cmd);
    queue->commitToGPUAndWait();

    // (a) Per-control-point output echoes the input control points.
    auto outReader = GEBufferReader::Create();
    outReader->setInputBuffer(hullOut);
    outReader->setStructLayout(cpLayout);
    for (unsigned i = 0; i < controlPointCount; ++i) {
        outReader->structBegin();
        auto v = FVec<4>::Create();
        outReader->getFloat4(v);
        outReader->structEnd();
        for (unsigned k = 0; k < 4; ++k) {
            if (v[k][0] != cpValues[i][k]) {
                std::cerr << "  FAIL hullOut[" << i << "][" << k << "] = " << v[k][0]
                          << " expected " << cpValues[i][k] << "\n";
                failFlag() = true;
            }
        }
    }
    outReader->reset();

    // (b) Factor buffer holds the expected half bit pattern in every slot.
    OmegaCommon::Vector<omegasl_data_type> factorLayout;
    for (unsigned w = 0; w < factorWordCount; ++w) factorLayout.push_back(OMEGASL_UINT);
    auto factorReader = GEBufferReader::Create();
    factorReader->setInputBuffer(factors);
    factorReader->setStructLayout(factorLayout);
    factorReader->structBegin();
    std::vector<unsigned> words(factorWordCount);
    for (unsigned w = 0; w < factorWordCount; ++w) factorReader->getUint(words[w]);
    factorReader->structEnd();
    factorReader->reset();

    // Every packed half in the factor struct is the same patchfn value, so both
    // the low and high 16-bit lanes of each word must equal factorHalfBits.
    for (unsigned w = 0; w < factorWordCount; ++w) {
        expectHalfBits("factor.lo", words[w], factorHalfBits);
        expectHalfBits("factor.hi", words[w] >> 16, factorHalfBits);
    }

    if (!failFlag()) std::cout << "  PASS\n";
}

}  // namespace

GTE_TEST_ENTRY_POINT {
    (void)argc;
    (void)argv;
    auto gte = OmegaGTE::InitWithDefaultDevice();

    auto compiled = gte.omegaSlCompiler->compile({OmegaSLCompiler::Source::fromString(kShaders)});
    auto lib = gte.graphicsEngine->loadShaderLibraryRuntime(compiled);

    OmegaGTE::GECommandQueueDesc queueDesc{};
    queueDesc.maxBufferCount = 4;
    auto queue = gte.graphicsEngine->makeCommandQueue(queueDesc);

    // Triangle: 3 control points, factors are MTLTriangleTessellationFactorsHalf
    // (3 edge + 1 inside half = 8 bytes = 2 words); patchfn writes 1.0 (0x3C00).
    runHull(gte, lib, queue, "triHull", 3, 2, 0x3C00u);

    // Quad: 4 control points, factors are MTLQuadTessellationFactorsHalf
    // (4 edge + 2 inside half = 12 bytes = 3 words); patchfn writes 2.0 (0x4000).
    runHull(gte, lib, queue, "quadHull", 4, 3, 0x4000u);

    OmegaGTE::Close(gte);

    const bool ok = !failFlag();
    std::cout << (ok ? "PASS: tessellation hull dispatch" : "FAIL: tessellation hull dispatch") << "\n";
    return ok ? 0 : 1;
}
