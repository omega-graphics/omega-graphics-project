#include "AQComputeBackend.h"
#include "AQBodySoA.h"

// Phase 5b: AQUA now links OmegaGTE and defines the target-platform macro
// (aqua/CMakeLists.txt), so the GPU headers compile here. The Metal-specific
// Objective-C bits in GE.h are `#ifdef __OBJC__`-guarded, so this stays a .cpp —
// every call below is the public C++ GTE surface (no Metal types).
#include <omegaGTE/GE.h>             // OmegaGraphicsEngine, GEBuffer, BufferDescriptor
#include <omegaGTE/GEPipeline.h>     // ComputePipelineDescriptor, GEComputePipelineState
#include <omegaGTE/GERenderTarget.h> // GECommandBuffer compute pass
#include <omegaGTE/GECommandQueue.h> // GECommandQueue, GEComputePassDescriptor
#include <omegaGTE/GTEShader.h>      // GTEShaderLibrary, GEBufferWriter/Reader, omegaSLStructStride
#include <omegaGTE/GTEMath.h>        // FVec<4>
#include <omegasl.h>                 // OMEGASL_FLOAT4
#include <omega-common/fs.h>         // OmegaCommon::FS::Path

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <utility>

namespace {

// Host mirror of the kernels' shared push-constant block (AQStepParams in
// AQKernelsCommon.omegaslh): float4 gravityDt + uint4 counts, 32 bytes, no
// padding surprises (both fields are 16-byte groups).
struct AQStepParamsHost {
    float         gravityDt[4];
    std::uint32_t counts[4];
};

// Kernel buffer slots (AQIntegrate.omegasl). The push-constant block needs no
// slot at bind time (setComputeConstants resolves it from the pipeline), so
// only the body buffers appear here.
constexpr unsigned kSlotPosAct   = 1;
constexpr unsigned kSlotVelIm    = 2;
constexpr unsigned kSlotQuat     = 3;
constexpr unsigned kSlotWbMax    = 4;
constexpr unsigned kSlotInvIGs   = 5;
constexpr unsigned kSlotForceLd  = 6;
constexpr unsigned kSlotTorqueAd = 7;
constexpr unsigned kSlotPseudo   = 8;

// Broadphase kernel slots (AQBroadphase.omegasl).
constexpr unsigned kBpPosAct         = 1;
constexpr unsigned kBpQuat           = 2;
constexpr unsigned kBpVelIm          = 3;
constexpr unsigned kBpShapeInfo      = 4;
constexpr unsigned kBpShapeParams    = 5;
constexpr unsigned kBpShapeLocalPos  = 6;
constexpr unsigned kBpShapeLocalQuat = 7;
constexpr unsigned kBpHullVerts      = 8;
constexpr unsigned kBpFilter         = 9;
constexpr unsigned kBpWorldMin       = 10;
constexpr unsigned kBpWorldMax       = 11;
constexpr unsigned kBpFatMin         = 12;
constexpr unsigned kBpFatMax         = 13;
constexpr unsigned kBpEntries        = 14;
constexpr unsigned kBpCellCoords     = 15;
constexpr unsigned kBpEntryRanks     = 16;
constexpr unsigned kBpEntriesSorted  = 17;
constexpr unsigned kBpPairCount      = 18;
constexpr unsigned kBpPairs          = 19;
constexpr unsigned kBpPairRanks      = 20;
constexpr unsigned kBpPairsSorted    = 21;

// Host mirror of AQBroadphaseParams: config = (cellSize, fattenMargin,
// frameDt, unused); counts = (bodyCount, pairCapacity, unused, unused).
struct AQBroadphaseParamsHost {
    float         config[4];
    std::uint32_t counts[4];
};

// Narrowphase kernel slots (AQNarrowphase.omegasl).
constexpr unsigned kNpPosAct         = 1;
constexpr unsigned kNpQuat           = 2;
constexpr unsigned kNpVelIm          = 3;
constexpr unsigned kNpWbMax          = 4;
constexpr unsigned kNpInvIGs         = 5;
constexpr unsigned kNpCom            = 6;
constexpr unsigned kNpMaterial       = 7;
constexpr unsigned kNpShapeInfo      = 8;
constexpr unsigned kNpShapeParams    = 9;
constexpr unsigned kNpShapeLocalPos  = 10;
constexpr unsigned kNpShapeLocalQuat = 11;
constexpr unsigned kNpHullVerts      = 12;
constexpr unsigned kNpPairs          = 13;
constexpr unsigned kNpContactFlag    = 14;
constexpr unsigned kNpPointCount     = 15;
constexpr unsigned kNpCpuFallback    = 16;
constexpr unsigned kNpScanIn         = 17;
constexpr unsigned kNpScanOut        = 18;
constexpr unsigned kNpScanFlagFinal  = 19;
constexpr unsigned kNpScanPtsFinal   = 20;
constexpr unsigned kNpManifolds      = 21;
constexpr unsigned kNpRows           = 22;
constexpr unsigned kNpCacheKeys      = 23;
constexpr unsigned kNpCacheVals      = 24;

// Host mirror of AQNarrowphaseParams: config = (dt, 0, 0, 0);
// counts = (pairCount, cacheCount, combineModes, scanStride).
struct AQNarrowphaseParamsHost {
    float         config[4];
    std::uint32_t counts[4];
};

// Solver kernel slots (AQSolver.omegasl).
constexpr unsigned kSvPosAct        = 1;
constexpr unsigned kSvVelIm         = 2;
constexpr unsigned kSvQuat          = 3;
constexpr unsigned kSvWbMax         = 4;
constexpr unsigned kSvInvIGs        = 5;
constexpr unsigned kSvCom           = 6;
constexpr unsigned kSvRows          = 7;
constexpr unsigned kSvGroups        = 8;
constexpr unsigned kSvGroupsByColor = 9;
constexpr unsigned kSvManifolds     = 10;
constexpr unsigned kSvPseudoLin     = 11;
constexpr unsigned kSvPseudoAng     = 12;

// Host mirror of AQSolverParams: config = (dt, 0, 0, 0);
// counts = (colorStart, colorCount, mode, bodyCount).
struct AQSolverParamsHost {
    float         config[4];
    std::uint32_t counts[4];
};

// One float4 per body per component-group buffer.
std::size_t bodyBufferStride() {
    static const std::size_t stride = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4});
    return stride;
}

// Write `count` float4 records (x/y/z/w lane arrays) into `buffer`.
// A null lane pointer writes 0 in that lane.
bool writeF4Buffer(SharedHandle<OmegaGTE::GEBuffer>& buffer,
                   const float* x, const float* y, const float* z, const float* w,
                   std::size_t count) {
    auto writer = OmegaGTE::GEBufferWriter::Create();
    writer->setOutputBuffer(buffer);
    for (std::size_t i = 0; i < count; ++i) {
        auto v = OmegaGTE::FVec<4>::Create();
        v[0][0] = x ? x[i] : 0.f;
        v[1][0] = y ? y[i] : 0.f;
        v[2][0] = z ? z[i] : 0.f;
        v[3][0] = w ? w[i] : 0.f;
        writer->structBegin();
        writer->writeFloat4(v);
        writer->structEnd();
        writer->sendToBuffer();
    }
    writer->flush();
    return true;
}

// Read `count` float4 records out of `buffer` into lane arrays (null lane
// pointers discard that lane).
bool readF4Buffer(SharedHandle<OmegaGTE::GEBuffer>& buffer,
                  float* x, float* y, float* z, float* w,
                  std::size_t count) {
    auto reader = OmegaGTE::GEBufferReader::Create();
    reader->setInputBuffer(buffer);
    reader->setStructLayout({OMEGASL_FLOAT4});
    for (std::size_t i = 0; i < count; ++i) {
        reader->structBegin();
        auto v = OmegaGTE::FVec<4>::Create();
        reader->getFloat4(v);
        reader->structEnd();
        if (x) { x[i] = v[0][0]; }
        if (y) { y[i] = v[1][0]; }
        if (z) { z[i] = v[2][0]; }
        if (w) { w[i] = v[3][0]; }
    }
    return true;
}

} // namespace

AQComputeBackend::AQComputeBackend(SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
                                   SharedHandle<OmegaGTE::GECommandQueue> queue)
    : gpuEngine(std::move(engine)), cmdQueue(std::move(queue)) {}

AQComputeBackend::~AQComputeBackend() = default;

std::unique_ptr<AQComputeBackend> AQComputeBackend::TryCreate(
    SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
    SharedHandle<OmegaGTE::GECommandQueue> queue) {
    // No engine ⇒ CPU-only context: there is no device to dispatch kernels to.
    if (!engine) {
        return nullptr;
    }

    // Hold the engine + queue. `gpuUsable` stays false until the full GPU
    // step pipeline is ported (5f/5g); the stage kernels land behind the
    // stage-isolation parity tests first.
    return std::unique_ptr<AQComputeBackend>(
        new AQComputeBackend(std::move(engine), std::move(queue)));
}

bool AQComputeBackend::loadKernelLibrary(const OmegaCommon::String& path) {
    if (!gpuEngine) {
        return false;
    }
    kernelLib = gpuEngine->loadShaderLibrary(OmegaCommon::FS::Path(path));
    return static_cast<bool>(kernelLib);
}

SharedHandle<OmegaGTE::GEComputePipelineState> AQComputeBackend::pipeline(const std::string& name) {
    auto cached = pipelines.find(name);
    if (cached != pipelines.end()) {
        return cached->second;
    }
    if (!gpuEngine || !kernelLib) {
        std::cerr << "AQComputeBackend: pipeline(" << name
                  << ") requested before the kernel library was loaded\n";
        return nullptr;
    }
    auto it = kernelLib->shaders.find(name);
    if (it == kernelLib->shaders.end() || !it->second) {
        std::cerr << "AQComputeBackend: kernel `" << name
                  << "` not found in the loaded library\n";
        return nullptr;
    }
    OmegaGTE::ComputePipelineDescriptor desc;
    desc.name = name;
    desc.computeFunc = it->second;
    auto pso = gpuEngine->makeComputePipelineState(desc);
    if (!pso) {
        std::cerr << "AQComputeBackend: failed to build the `" << name
                  << "` compute pipeline\n";
        return nullptr;
    }
    pipelines.emplace(name, pso);
    return pso;
}

bool AQComputeBackend::ensureBodyCapacity(std::size_t bodyCount) {
    if (!gpuEngine) {
        return false;
    }
    if (bodyCount <= bodyCapacity) {
        return true;
    }
    // Geometric growth so a slowly-growing scene doesn't reallocate per step.
    std::size_t newCap = (bodyCapacity == 0) ? 64 : bodyCapacity;
    while (newCap < bodyCount) {
        newCap *= 2;
    }

    const std::size_t stride = bodyBufferStride();
    // The body pool is CPU-seeded, kernel-mutated, and CPU-read-back — the
    // exact shared-both-ways case `Universal` usage exists for (the backend
    // owns the staging; on D3D12 that is a DEFAULT primary + both
    // companions). Input-only buffers (invIGs, com) are plain Upload — the
    // kernels never write them, so the CPU writes them in place.
    OmegaGTE::BufferDescriptor uniDesc{OmegaGTE::BufferDescriptor::Universal,
                                       newCap * stride, stride};
    OmegaGTE::BufferDescriptor upDesc{OmegaGTE::BufferDescriptor::Upload,
                                      newCap * stride, stride};
    auto make = [&](SharedHandle<OmegaGTE::GEBuffer>& slot,
                    const OmegaGTE::BufferDescriptor& desc) {
        slot = gpuEngine->makeBuffer(desc);
        return static_cast<bool>(slot);
    };
    if (!make(bodyPosAct, uniDesc) || !make(bodyVelIm, uniDesc) ||
        !make(bodyQuat, uniDesc) || !make(bodyWbMax, uniDesc) ||
        !make(bodyForceLd, uniDesc) || !make(bodyTorqueAd, uniDesc) ||
        !make(bodyPseudo, uniDesc) || !make(bodyPseudoAng, uniDesc) ||
        !make(bodyInvIGs, upDesc) || !make(bodyCom, upDesc)) {
        std::cerr << "AQComputeBackend: body-buffer allocation failed at capacity "
                  << newCap << "\n";
        bodyCapacity = 0;
        return false;
    }
    bodyCapacity = newCap;
    return true;
}

bool AQComputeBackend::uploadBodies(const AQBodySoA& soa) {
    const std::size_t n = soa.size();
    if (!ensureBodyCapacity(n)) {
        return false;
    }
    if (n == 0) {
        return true;
    }

    // activation rides the posAct w lane as an exact small-integer float.
    OmegaCommon::Vector<float> act(n);
    for (std::size_t i = 0; i < n; ++i) {
        act[i] = static_cast<float>(soa.activation[i]);
    }

    // Universal buffers accept the CPU write directly — the backend stages it
    // and flushes into the GPU-resident primary at the next bind.
    return writeF4Buffer(bodyPosAct, soa.posX.data(), soa.posY.data(), soa.posZ.data(), act.data(), n) &&
           writeF4Buffer(bodyVelIm, soa.velX.data(), soa.velY.data(), soa.velZ.data(), soa.invMass.data(), n) &&
           writeF4Buffer(bodyQuat, soa.quatX.data(), soa.quatY.data(), soa.quatZ.data(), soa.quatW.data(), n) &&
           writeF4Buffer(bodyWbMax, soa.wbX.data(), soa.wbY.data(), soa.wbZ.data(), soa.maxAngularSpeed.data(), n) &&
           writeF4Buffer(bodyInvIGs, soa.invInertiaX.data(), soa.invInertiaY.data(), soa.invInertiaZ.data(), soa.gravityScale.data(), n) &&
           writeF4Buffer(bodyForceLd, soa.forceX.data(), soa.forceY.data(), soa.forceZ.data(), soa.linearDamping.data(), n) &&
           writeF4Buffer(bodyTorqueAd, soa.torqueX.data(), soa.torqueY.data(), soa.torqueZ.data(), soa.angularDamping.data(), n) &&
           writeF4Buffer(bodyPseudo, soa.pseudoLinX.data(), soa.pseudoLinY.data(), soa.pseudoLinZ.data(), nullptr, n) &&
           writeF4Buffer(bodyCom, soa.comX.data(), soa.comY.data(), soa.comZ.data(), nullptr, n);
}

bool AQComputeBackend::downloadBodies(AQBodySoA& soa) {
    const std::size_t n = soa.size();
    if (n == 0) {
        return true;
    }
    if (n > bodyCapacity || !bodyPosAct) {
        std::cerr << "AQComputeBackend::downloadBodies: nothing uploaded for "
                  << n << " bodies\n";
        return false;
    }

    OmegaCommon::Vector<float> act(n);
    bool ok =
        readF4Buffer(bodyPosAct, soa.posX.data(), soa.posY.data(), soa.posZ.data(), act.data(), n) &&
        readF4Buffer(bodyVelIm, soa.velX.data(), soa.velY.data(), soa.velZ.data(), soa.invMass.data(), n) &&
        readF4Buffer(bodyQuat, soa.quatX.data(), soa.quatY.data(), soa.quatZ.data(), soa.quatW.data(), n) &&
        readF4Buffer(bodyWbMax, soa.wbX.data(), soa.wbY.data(), soa.wbZ.data(), soa.maxAngularSpeed.data(), n) &&
        readF4Buffer(bodyInvIGs, soa.invInertiaX.data(), soa.invInertiaY.data(), soa.invInertiaZ.data(), soa.gravityScale.data(), n) &&
        readF4Buffer(bodyForceLd, soa.forceX.data(), soa.forceY.data(), soa.forceZ.data(), soa.linearDamping.data(), n) &&
        readF4Buffer(bodyTorqueAd, soa.torqueX.data(), soa.torqueY.data(), soa.torqueZ.data(), soa.angularDamping.data(), n) &&
        readF4Buffer(bodyPseudo, soa.pseudoLinX.data(), soa.pseudoLinY.data(), soa.pseudoLinZ.data(), nullptr, n);
    if (!ok) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        soa.activation[i] = static_cast<std::uint8_t>(act[i]);
    }
    return true;
}

bool AQComputeBackend::encodeIntegrate(float dt, const float gravity[3],
                                       std::size_t bodyCount, unsigned substeps) {
    if (!gpuEngine || !cmdQueue) {
        std::cerr << "AQComputeBackend::encodeIntegrate: engine or queue is null\n";
        return false;
    }
    if (bodyCount == 0 || substeps == 0) {
        return true;
    }
    if (bodyCount > bodyCapacity) {
        std::cerr << "AQComputeBackend::encodeIntegrate: " << bodyCount
                  << " bodies but only " << bodyCapacity << " uploaded\n";
        return false;
    }

    auto velPipe = pipeline("AQIntegrateVelocity");
    auto posPipe = pipeline("AQIntegratePosition");
    if (!velPipe || !posPipe) {
        return false;
    }

    AQStepParamsHost params{};
    params.gravityDt[0] = gravity[0];
    params.gravityDt[1] = gravity[1];
    params.gravityDt[2] = gravity[2];
    params.gravityDt[3] = dt;
    params.counts[0] = static_cast<std::uint32_t>(bodyCount);

    const unsigned threads = static_cast<unsigned>(bodyCount);

    // All sub-steps ride ONE command buffer — sequential compute passes give
    // the velocity->position->velocity ordering within it, and a single
    // commit + wait at the end is the per-`advance` sync point.
    auto cmdBuf = cmdQueue->getAvailableBuffer();
    for (unsigned s = 0; s < substeps; ++s) {
        OmegaGTE::GEComputePassDescriptor passDesc;
        cmdBuf->startComputePass(passDesc);
        cmdBuf->setComputePipelineState(velPipe);
        cmdBuf->bindResourceAtComputeShader(bodyPosAct, kSlotPosAct);
        cmdBuf->bindResourceAtComputeShader(bodyVelIm, kSlotVelIm);
        cmdBuf->bindResourceAtComputeShader(bodyQuat, kSlotQuat);
        cmdBuf->bindResourceAtComputeShader(bodyWbMax, kSlotWbMax);
        cmdBuf->bindResourceAtComputeShader(bodyInvIGs, kSlotInvIGs);
        cmdBuf->bindResourceAtComputeShader(bodyForceLd, kSlotForceLd);
        cmdBuf->bindResourceAtComputeShader(bodyTorqueAd, kSlotTorqueAd);
        cmdBuf->setComputeConstants(&params, sizeof(params));
        cmdBuf->dispatchThreads(threads, 1, 1);
        cmdBuf->finishComputePass();

        OmegaGTE::GEComputePassDescriptor posPassDesc;
        cmdBuf->startComputePass(posPassDesc);
        cmdBuf->setComputePipelineState(posPipe);
        cmdBuf->bindResourceAtComputeShader(bodyPosAct, kSlotPosAct);
        cmdBuf->bindResourceAtComputeShader(bodyVelIm, kSlotVelIm);
        cmdBuf->bindResourceAtComputeShader(bodyQuat, kSlotQuat);
        cmdBuf->bindResourceAtComputeShader(bodyWbMax, kSlotWbMax);
        cmdBuf->bindResourceAtComputeShader(bodyForceLd, kSlotForceLd);
        cmdBuf->bindResourceAtComputeShader(bodyTorqueAd, kSlotTorqueAd);
        cmdBuf->bindResourceAtComputeShader(bodyPseudo, kSlotPseudo);
        cmdBuf->setComputeConstants(&params, sizeof(params));
        cmdBuf->dispatchThreads(threads, 1, 1);
        cmdBuf->finishComputePass();
    }
    cmdQueue->submitCommandBuffer(cmdBuf);
    cmdQueue->commitToGPUAndWait();
    return true;
}

// ============================================================================
// Phase 5d — broadphase.
// ============================================================================

bool AQComputeBackend::ensureBroadphaseCapacity(std::size_t bodyCount,
                                                std::size_t hullVertCount) {
    if (!gpuEngine) {
        return false;
    }
    const std::size_t f4 = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4});
    const std::size_t u4 = OmegaGTE::omegaSLStructStride({OMEGASL_UINT4});
    const std::size_t i4 = OmegaGTE::omegaSLStructStride({OMEGASL_INT4});
    const std::size_t u2 = OmegaGTE::omegaSLStructStride({OMEGASL_UINT2});
    const std::size_t u1 = OmegaGTE::omegaSLStructStride({OMEGASL_UINT});

    // Kernel outputs (AABBs, grid entries, ranks, pairs, the counter) are
    // `Readback` usage — GPU-writable residency with the CPU read path the
    // downloads use. The shape/filter tables are CPU-owned inputs → Upload.
    auto grow = [&](SharedHandle<OmegaGTE::GEBuffer>& slot, std::size_t bytes,
                    std::size_t stride,
                    OmegaGTE::BufferDescriptor::Usage usage =
                        OmegaGTE::BufferDescriptor::Upload) {
        OmegaGTE::BufferDescriptor desc{usage, bytes, stride};
        slot = gpuEngine->makeBuffer(desc);
        return static_cast<bool>(slot);
    };
    const auto kGpu = OmegaGTE::BufferDescriptor::Readback;

    if (bodyCount > bpBodyCapacity) {
        std::size_t cap = (bpBodyCapacity == 0) ? 64 : bpBodyCapacity;
        while (cap < bodyCount) { cap *= 2; }
        bool ok = grow(bpShapeInfo, cap * u4, u4) &&
                  grow(bpShapeParams, cap * f4, f4) &&
                  grow(bpShapeLocalPos, cap * f4, f4) &&
                  grow(bpShapeLocalQuat, cap * f4, f4) &&
                  grow(bpFilter, cap * u4, u4) &&
                  grow(bpWorldMin, cap * f4, f4, kGpu) && grow(bpWorldMax, cap * f4, f4, kGpu) &&
                  grow(bpFatMin, cap * f4, f4, kGpu) && grow(bpFatMax, cap * f4, f4, kGpu) &&
                  grow(bpEntries, cap * u2, u2, kGpu) && grow(bpEntriesSorted, cap * u2, u2, kGpu) &&
                  grow(bpCellCoords, cap * i4, i4, kGpu) &&
                  grow(bpEntryRanks, cap * u1, u1, kGpu);
        if (!ok) {
            std::cerr << "AQComputeBackend: broadphase body-pool allocation failed\n";
            bpBodyCapacity = 0;
            return false;
        }
        bpBodyCapacity = cap;
    }
    // CPU-zeroed each chain run + GPU-appended → Universal.
    if (!bpPairCount &&
        !grow(bpPairCount, u1, u1, OmegaGTE::BufferDescriptor::Universal)) {
        return false;
    }
    // The hull pool can be empty; allocate at least one slot so the buffer
    // binding is always valid.
    const std::size_t wantVerts = (hullVertCount == 0) ? 1 : hullVertCount;
    if (wantVerts > bpHullVertCapacity) {
        std::size_t cap = (bpHullVertCapacity == 0) ? 64 : bpHullVertCapacity;
        while (cap < wantVerts) { cap *= 2; }
        if (!grow(bpHullVerts, cap * f4, f4)) {
            return false;
        }
        bpHullVertCapacity = cap;
    }
    return true;
}

bool AQComputeBackend::ensurePairCapacity(std::size_t pairCapacity) {
    if (pairCapacity <= bpPairCapacity) {
        return true;
    }
    const std::size_t u2 = OmegaGTE::omegaSLStructStride({OMEGASL_UINT2});
    const std::size_t u1 = OmegaGTE::omegaSLStructStride({OMEGASL_UINT});
    std::size_t cap = (bpPairCapacity == 0) ? 256 : bpPairCapacity;
    while (cap < pairCapacity) { cap *= 2; }
    // All three are kernel outputs → Readback usage (GPU-writable residency).
    OmegaGTE::BufferDescriptor pairDesc{OmegaGTE::BufferDescriptor::Readback, cap * u2, u2};
    OmegaGTE::BufferDescriptor rankDesc{OmegaGTE::BufferDescriptor::Readback, cap * u1, u1};
    bpPairs = gpuEngine->makeBuffer(pairDesc);
    bpPairsSorted = gpuEngine->makeBuffer(pairDesc);
    bpPairRanks = gpuEngine->makeBuffer(rankDesc);
    if (!bpPairs || !bpPairsSorted || !bpPairRanks) {
        std::cerr << "AQComputeBackend: pair-pool allocation failed at " << cap << "\n";
        bpPairCapacity = 0;
        return false;
    }
    bpPairCapacity = cap;
    return true;
}

bool AQComputeBackend::uploadBroadphaseInputs(const BroadphaseInputs& in) {
    const std::size_t n = in.size();
    const std::size_t hv = in.hullVertX.size();
    if (!ensureBroadphaseCapacity(n, hv)) {
        return false;
    }
    if (n == 0) {
        return true;
    }

    // shapeInfo: (type, firstVert, vertCount, flags bit0=hasShape).
    {
        auto writer = OmegaGTE::GEBufferWriter::Create();
        writer->setOutputBuffer(bpShapeInfo);
        for (std::size_t i = 0; i < n; ++i) {
            auto v = OmegaGTE::UVec<4>::Create();
            v[0][0] = in.shapeType[i];
            v[1][0] = in.hullFirst[i];
            v[2][0] = in.hullCount[i];
            v[3][0] = in.hasShape[i];
            writer->structBegin();
            writer->writeUint4(v);
            writer->structEnd();
            writer->sendToBuffer();
        }
        writer->flush();
    }
    // filter: (layer, mask, 0, 0).
    {
        auto writer = OmegaGTE::GEBufferWriter::Create();
        writer->setOutputBuffer(bpFilter);
        for (std::size_t i = 0; i < n; ++i) {
            auto v = OmegaGTE::UVec<4>::Create();
            v[0][0] = in.filterLayer[i];
            v[1][0] = in.filterMask[i];
            writer->structBegin();
            writer->writeUint4(v);
            writer->structEnd();
            writer->sendToBuffer();
        }
        writer->flush();
    }
    bool ok = writeF4Buffer(bpShapeParams, in.paramX.data(), in.paramY.data(),
                            in.paramZ.data(), in.paramW.data(), n) &&
              writeF4Buffer(bpShapeLocalPos, in.localPosX.data(), in.localPosY.data(),
                            in.localPosZ.data(), nullptr, n) &&
              writeF4Buffer(bpShapeLocalQuat, in.localQuatX.data(), in.localQuatY.data(),
                            in.localQuatZ.data(), in.localQuatW.data(), n);
    if (ok && hv > 0) {
        ok = writeF4Buffer(bpHullVerts, in.hullVertX.data(), in.hullVertY.data(),
                           in.hullVertZ.data(), nullptr, hv);
    }
    return ok;
}

bool AQComputeBackend::encodeRefreshAABB(std::size_t bodyCount, float fattenMargin,
                                         float frameDt) {
    if (!gpuEngine || !cmdQueue || bodyCount == 0) {
        return bodyCount == 0;
    }
    if (bodyCount > bodyCapacity || bodyCount > bpBodyCapacity) {
        std::cerr << "AQComputeBackend::encodeRefreshAABB: inputs not uploaded for "
                  << bodyCount << " bodies\n";
        return false;
    }
    auto pso = pipeline("AQRefreshAABB");
    if (!pso) {
        return false;
    }

    AQBroadphaseParamsHost params{};
    params.config[1] = fattenMargin;
    params.config[2] = frameDt;
    params.counts[0] = static_cast<std::uint32_t>(bodyCount);

    auto cmdBuf = cmdQueue->getAvailableBuffer();
    OmegaGTE::GEComputePassDescriptor passDesc;
    cmdBuf->startComputePass(passDesc);
    cmdBuf->setComputePipelineState(pso);
    cmdBuf->bindResourceAtComputeShader(bodyPosAct, kBpPosAct);
    cmdBuf->bindResourceAtComputeShader(bodyQuat, kBpQuat);
    cmdBuf->bindResourceAtComputeShader(bodyVelIm, kBpVelIm);
    cmdBuf->bindResourceAtComputeShader(bpShapeInfo, kBpShapeInfo);
    cmdBuf->bindResourceAtComputeShader(bpShapeParams, kBpShapeParams);
    cmdBuf->bindResourceAtComputeShader(bpShapeLocalPos, kBpShapeLocalPos);
    cmdBuf->bindResourceAtComputeShader(bpShapeLocalQuat, kBpShapeLocalQuat);
    cmdBuf->bindResourceAtComputeShader(bpHullVerts, kBpHullVerts);
    cmdBuf->bindResourceAtComputeShader(bpWorldMin, kBpWorldMin);
    cmdBuf->bindResourceAtComputeShader(bpWorldMax, kBpWorldMax);
    cmdBuf->bindResourceAtComputeShader(bpFatMin, kBpFatMin);
    cmdBuf->bindResourceAtComputeShader(bpFatMax, kBpFatMax);
    cmdBuf->setComputeConstants(&params, sizeof(params));
    cmdBuf->dispatchThreads(static_cast<unsigned>(bodyCount), 1, 1);
    cmdBuf->finishComputePass();
    cmdQueue->submitCommandBuffer(cmdBuf);
    cmdQueue->commitToGPUAndWait();
    return true;
}

bool AQComputeBackend::downloadFatAABBs(OmegaCommon::Vector<float>& minXYZ,
                                        OmegaCommon::Vector<float>& maxXYZ,
                                        std::size_t bodyCount) {
    if (bodyCount == 0) {
        minXYZ.clear();
        maxXYZ.clear();
        return true;
    }
    if (bodyCount > bpBodyCapacity) {
        return false;
    }
    OmegaCommon::Vector<float> x(bodyCount), y(bodyCount), z(bodyCount);
    minXYZ.resize(bodyCount * 3);
    maxXYZ.resize(bodyCount * 3);
    if (!readF4Buffer(bpFatMin, x.data(), y.data(), z.data(), nullptr, bodyCount)) {
        return false;
    }
    for (std::size_t i = 0; i < bodyCount; ++i) {
        minXYZ[i * 3 + 0] = x[i];
        minXYZ[i * 3 + 1] = y[i];
        minXYZ[i * 3 + 2] = z[i];
    }
    if (!readF4Buffer(bpFatMax, x.data(), y.data(), z.data(), nullptr, bodyCount)) {
        return false;
    }
    for (std::size_t i = 0; i < bodyCount; ++i) {
        maxXYZ[i * 3 + 0] = x[i];
        maxXYZ[i * 3 + 1] = y[i];
        maxXYZ[i * 3 + 2] = z[i];
    }
    return true;
}

bool AQComputeBackend::runBroadphaseChain(std::size_t bodyCount, float cellSize,
                                          std::size_t pairCapacity,
                                          std::uint32_t& rawCount) {
    auto hashPso = pipeline("AQGridHash");
    auto rankPso = pipeline("AQSortEntriesRank");
    auto scatPso = pipeline("AQSortEntriesScatter");
    auto pairPso = pipeline("AQBroadphasePairs");
    auto prnkPso = pipeline("AQSortPairsRank");
    auto psctPso = pipeline("AQSortPairsScatter");
    if (!hashPso || !rankPso || !scatPso || !pairPso || !prnkPso || !psctPso) {
        return false;
    }

    // Zero the append counter (Universal buffer — the CPU write stages in the
    // backend and lands in the GPU-resident counter at its first bind below).
    {
        auto writer = OmegaGTE::GEBufferWriter::Create();
        writer->setOutputBuffer(bpPairCount);
        unsigned zero = 0;
        writer->structBegin();
        writer->writeUint(zero);
        writer->structEnd();
        writer->sendToBuffer();
        writer->flush();
    }

    AQBroadphaseParamsHost params{};
    params.config[0] = cellSize;
    params.counts[0] = static_cast<std::uint32_t>(bodyCount);
    params.counts[1] = static_cast<std::uint32_t>(pairCapacity);

    const unsigned nThreads = static_cast<unsigned>(bodyCount);
    const unsigned pairThreads = static_cast<unsigned>(pairCapacity);

    auto cmdBuf = cmdQueue->getAvailableBuffer();
    {
        OmegaGTE::GEComputePassDescriptor d;
        cmdBuf->startComputePass(d);
        cmdBuf->setComputePipelineState(hashPso);
        cmdBuf->bindResourceAtComputeShader(bpShapeInfo, kBpShapeInfo);
        cmdBuf->bindResourceAtComputeShader(bpFatMin, kBpFatMin);
        cmdBuf->bindResourceAtComputeShader(bpFatMax, kBpFatMax);
        cmdBuf->bindResourceAtComputeShader(bpEntries, kBpEntries);
        cmdBuf->bindResourceAtComputeShader(bpCellCoords, kBpCellCoords);
        cmdBuf->setComputeConstants(&params, sizeof(params));
        cmdBuf->dispatchThreads(nThreads, 1, 1);
        cmdBuf->finishComputePass();
    }
    {
        OmegaGTE::GEComputePassDescriptor d;
        cmdBuf->startComputePass(d);
        cmdBuf->setComputePipelineState(rankPso);
        cmdBuf->bindResourceAtComputeShader(bpEntries, kBpEntries);
        cmdBuf->bindResourceAtComputeShader(bpEntryRanks, kBpEntryRanks);
        cmdBuf->setComputeConstants(&params, sizeof(params));
        cmdBuf->dispatchThreads(nThreads, 1, 1);
        cmdBuf->finishComputePass();
    }
    {
        OmegaGTE::GEComputePassDescriptor d;
        cmdBuf->startComputePass(d);
        cmdBuf->setComputePipelineState(scatPso);
        cmdBuf->bindResourceAtComputeShader(bpEntries, kBpEntries);
        cmdBuf->bindResourceAtComputeShader(bpEntryRanks, kBpEntryRanks);
        cmdBuf->bindResourceAtComputeShader(bpEntriesSorted, kBpEntriesSorted);
        cmdBuf->setComputeConstants(&params, sizeof(params));
        cmdBuf->dispatchThreads(nThreads, 1, 1);
        cmdBuf->finishComputePass();
    }
    {
        OmegaGTE::GEComputePassDescriptor d;
        cmdBuf->startComputePass(d);
        cmdBuf->setComputePipelineState(pairPso);
        cmdBuf->bindResourceAtComputeShader(bpShapeInfo, kBpShapeInfo);
        cmdBuf->bindResourceAtComputeShader(bpFilter, kBpFilter);
        cmdBuf->bindResourceAtComputeShader(bpFatMin, kBpFatMin);
        cmdBuf->bindResourceAtComputeShader(bpFatMax, kBpFatMax);
        cmdBuf->bindResourceAtComputeShader(bpCellCoords, kBpCellCoords);
        cmdBuf->bindResourceAtComputeShader(bpEntriesSorted, kBpEntriesSorted);
        cmdBuf->bindResourceAtComputeShader(bpPairCount, kBpPairCount);
        cmdBuf->bindResourceAtComputeShader(bpPairs, kBpPairs);
        cmdBuf->setComputeConstants(&params, sizeof(params));
        cmdBuf->dispatchThreads(nThreads, 1, 1);
        cmdBuf->finishComputePass();
    }
    {
        OmegaGTE::GEComputePassDescriptor d;
        cmdBuf->startComputePass(d);
        cmdBuf->setComputePipelineState(prnkPso);
        cmdBuf->bindResourceAtComputeShader(bpPairs, kBpPairs);
        cmdBuf->bindResourceAtComputeShader(bpPairCount, kBpPairCount);
        cmdBuf->bindResourceAtComputeShader(bpPairRanks, kBpPairRanks);
        cmdBuf->setComputeConstants(&params, sizeof(params));
        cmdBuf->dispatchThreads(pairThreads, 1, 1);
        cmdBuf->finishComputePass();
    }
    {
        OmegaGTE::GEComputePassDescriptor d;
        cmdBuf->startComputePass(d);
        cmdBuf->setComputePipelineState(psctPso);
        cmdBuf->bindResourceAtComputeShader(bpPairs, kBpPairs);
        cmdBuf->bindResourceAtComputeShader(bpPairCount, kBpPairCount);
        cmdBuf->bindResourceAtComputeShader(bpPairRanks, kBpPairRanks);
        cmdBuf->bindResourceAtComputeShader(bpPairsSorted, kBpPairsSorted);
        cmdBuf->setComputeConstants(&params, sizeof(params));
        cmdBuf->dispatchThreads(pairThreads, 1, 1);
        cmdBuf->finishComputePass();
    }
    cmdQueue->submitCommandBuffer(cmdBuf);
    cmdQueue->commitToGPUAndWait();

    // Read the raw (pre-clamp) append count.
    auto reader = OmegaGTE::GEBufferReader::Create();
    reader->setInputBuffer(bpPairCount);
    reader->setStructLayout({OMEGASL_UINT});
    reader->structBegin();
    reader->getUint(rawCount);
    reader->structEnd();
    return true;
}

bool AQComputeBackend::encodeBroadphase(std::size_t bodyCount, float cellSize,
                                        std::size_t pairCapacityHint) {
    if (!gpuEngine || !cmdQueue) {
        return false;
    }
    if (bodyCount == 0) {
        return true;
    }
    if (bodyCount > bpBodyCapacity) {
        std::cerr << "AQComputeBackend::encodeBroadphase: inputs not uploaded\n";
        return false;
    }
    std::size_t capacity = (pairCapacityHint == 0) ? 256 : pairCapacityHint;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (!ensurePairCapacity(capacity)) {
            return false;
        }
        std::uint32_t rawCount = 0;
        if (!runBroadphaseChain(bodyCount, cellSize, bpPairCapacity, rawCount)) {
            return false;
        }
        if (rawCount <= bpPairCapacity) {
            return true;
        }
        // Overflow: the append counter tells us the real demand — grow and
        // re-run the (stateless) chain.
        capacity = rawCount;
    }
    std::cerr << "AQComputeBackend::encodeBroadphase: pair pool kept overflowing\n";
    return false;
}

bool AQComputeBackend::downloadPairs(OmegaCommon::Vector<std::uint32_t>& pairsAB) {
    pairsAB.clear();
    if (!bpPairCount) {
        return false;
    }
    std::uint32_t count = 0;
    {
        auto reader = OmegaGTE::GEBufferReader::Create();
        reader->setInputBuffer(bpPairCount);
        reader->setStructLayout({OMEGASL_UINT});
        reader->structBegin();
        reader->getUint(count);
        reader->structEnd();
    }
    if (count > bpPairCapacity) {
        std::cerr << "AQComputeBackend::downloadPairs: overflowed pair pool ("
                  << count << " > " << bpPairCapacity << ")\n";
        return false;
    }
    if (count == 0) {
        return true;
    }
    pairsAB.resize(static_cast<std::size_t>(count) * 2);
    auto reader = OmegaGTE::GEBufferReader::Create();
    reader->setInputBuffer(bpPairsSorted);
    reader->setStructLayout({OMEGASL_UINT2});
    for (std::uint32_t i = 0; i < count; ++i) {
        auto v = OmegaGTE::UVec<2>::Create();
        reader->structBegin();
        reader->getUint2(v);
        reader->structEnd();
        pairsAB[i * 2 + 0] = v[0][0];
        pairsAB[i * 2 + 1] = v[1][0];
    }
    return true;
}

// ============================================================================
// Phase 5e — narrowphase.
// ============================================================================

bool AQComputeBackend::ensureNarrowphaseCapacity(std::size_t pairCount) {
    if (!gpuEngine) {
        return false;
    }
    if (pairCount <= npPairCapacity && npMaterial) {
        return true;
    }
    const std::size_t f4 = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4});
    const std::size_t u1 = OmegaGTE::omegaSLStructStride({OMEGASL_UINT});
    // Manifold record: uint4 + 10 x float4 + uint4 (192 B, std430).
    const std::size_t manifoldStride = 12 * 16;
    // Row record: uint4 + 6 x float4 (112 B).
    const std::size_t rowStride = 7 * 16;

    auto grow = [&](SharedHandle<OmegaGTE::GEBuffer>& slot, std::size_t bytes,
                    std::size_t stride,
                    OmegaGTE::BufferDescriptor::Usage usage =
                        OmegaGTE::BufferDescriptor::Upload) {
        OmegaGTE::BufferDescriptor desc{usage, bytes, stride};
        slot = gpuEngine->makeBuffer(desc);
        return static_cast<bool>(slot);
    };
    const auto kGpu = OmegaGTE::BufferDescriptor::Readback;

    // Per-body material table rides body capacity (grown by uploadBodies
    // first; allocate at the pool's current capacity). Input-only → Upload.
    if (!npMaterial && bodyCapacity > 0) {
        if (!grow(npMaterial, bodyCapacity * f4, f4)) {
            return false;
        }
    }

    // Everything below is written by the count/scan/build kernels → Readback.
    std::size_t cap = (npPairCapacity == 0) ? 64 : npPairCapacity;
    while (cap < pairCount) { cap *= 2; }
    bool ok = grow(npContactFlag, cap * u1, u1, kGpu) &&
              grow(npPointCount, cap * u1, u1, kGpu) &&
              grow(npCpuFallback, cap * u1, u1, kGpu) &&
              grow(npScanA, cap * u1, u1, kGpu) && grow(npScanB, cap * u1, u1, kGpu) &&
              grow(npScanC, cap * u1, u1, kGpu) && grow(npScanD, cap * u1, u1, kGpu) &&
              grow(npManifolds, cap * manifoldStride, manifoldStride, kGpu) &&
              grow(npRows, cap * 12 * rowStride, rowStride, kGpu);
    if (!ok) {
        std::cerr << "AQComputeBackend: narrowphase pool allocation failed at "
                  << cap << " pairs\n";
        npPairCapacity = 0;
        return false;
    }
    npPairCapacity = cap;
    return true;
}

bool AQComputeBackend::uploadNarrowphaseInputs(
    const OmegaCommon::Vector<float>& restitution,
    const OmegaCommon::Vector<float>& friction,
    const OmegaCommon::Vector<std::uint32_t>& isTrigger) {
    const std::size_t n = restitution.size();
    if (n > bodyCapacity) {
        std::cerr << "AQComputeBackend::uploadNarrowphaseInputs: call uploadBodies first\n";
        return false;
    }
    const std::size_t f4 = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4});
    if (!npMaterial) {
        OmegaGTE::BufferDescriptor desc{OmegaGTE::BufferDescriptor::Upload,
                                        bodyCapacity * f4, f4};
        npMaterial = gpuEngine->makeBuffer(desc);
        if (!npMaterial) {
            return false;
        }
    }
    OmegaCommon::Vector<float> trig(n);
    for (std::size_t i = 0; i < n; ++i) {
        trig[i] = static_cast<float>(isTrigger[i]);
    }
    return writeF4Buffer(npMaterial, restitution.data(), friction.data(),
                         trig.data(), nullptr, n);
}

bool AQComputeBackend::uploadWarmStartCache(
    const OmegaCommon::Vector<std::uint32_t>& keysLoHi,
    const OmegaCommon::Vector<float>& valsNF0F1) {
    const std::size_t count = keysLoHi.size() / 2;
    if (count == 0) {
        return true;
    }
    const std::size_t f4 = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4});
    const std::size_t u2 = OmegaGTE::omegaSLStructStride({OMEGASL_UINT2});
    if (count > npCacheCapacity) {
        std::size_t cap = (npCacheCapacity == 0) ? 64 : npCacheCapacity;
        while (cap < count) { cap *= 2; }
        OmegaGTE::BufferDescriptor keyDesc{OmegaGTE::BufferDescriptor::Upload, cap * u2, u2};
        OmegaGTE::BufferDescriptor valDesc{OmegaGTE::BufferDescriptor::Upload, cap * f4, f4};
        npCacheKeys = gpuEngine->makeBuffer(keyDesc);
        npCacheVals = gpuEngine->makeBuffer(valDesc);
        if (!npCacheKeys || !npCacheVals) {
            npCacheCapacity = 0;
            return false;
        }
        npCacheCapacity = cap;
    }
    {
        auto writer = OmegaGTE::GEBufferWriter::Create();
        writer->setOutputBuffer(npCacheKeys);
        for (std::size_t i = 0; i < count; ++i) {
            auto v = OmegaGTE::UVec<2>::Create();
            v[0][0] = keysLoHi[i * 2 + 0];
            v[1][0] = keysLoHi[i * 2 + 1];
            writer->structBegin();
            writer->writeUint2(v);
            writer->structEnd();
            writer->sendToBuffer();
        }
        writer->flush();
    }
    OmegaCommon::Vector<float> vx(count), vy(count), vz(count);
    for (std::size_t i = 0; i < count; ++i) {
        vx[i] = valsNF0F1[i * 3 + 0];
        vy[i] = valsNF0F1[i * 3 + 1];
        vz[i] = valsNF0F1[i * 3 + 2];
    }
    return writeF4Buffer(npCacheVals, vx.data(), vy.data(), vz.data(), nullptr, count);
}

bool AQComputeBackend::encodeNarrowphase(std::size_t pairCount, float dt,
                                         std::uint32_t restCombineMode,
                                         std::uint32_t fricCombineMode,
                                         std::size_t cacheCount) {
    if (!gpuEngine || !cmdQueue) {
        return false;
    }
    if (pairCount == 0) {
        return true;
    }
    if (!ensureNarrowphaseCapacity(pairCount) || !npMaterial) {
        std::cerr << "AQComputeBackend::encodeNarrowphase: pools not ready "
                     "(uploadNarrowphaseInputs first)\n";
        return false;
    }
    if (cacheCount > 0 && (!npCacheKeys || cacheCount > npCacheCapacity)) {
        std::cerr << "AQComputeBackend::encodeNarrowphase: warm-start cache not uploaded\n";
        return false;
    }
    // The cache buffers must exist for binding even when running cold.
    if (!npCacheKeys) {
        const std::size_t f4 = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4});
        const std::size_t u2 = OmegaGTE::omegaSLStructStride({OMEGASL_UINT2});
        OmegaGTE::BufferDescriptor keyDesc{OmegaGTE::BufferDescriptor::Upload, 64 * u2, u2};
        OmegaGTE::BufferDescriptor valDesc{OmegaGTE::BufferDescriptor::Upload, 64 * f4, f4};
        npCacheKeys = gpuEngine->makeBuffer(keyDesc);
        npCacheVals = gpuEngine->makeBuffer(valDesc);
        npCacheCapacity = 64;
        if (!npCacheKeys || !npCacheVals) {
            return false;
        }
    }

    auto countPso = pipeline("AQNarrowphaseCount");
    auto scanPso = pipeline("AQPrefixScan");
    auto buildPso = pipeline("AQNarrowphaseBuild");
    if (!countPso || !scanPso || !buildPso) {
        return false;
    }

    AQNarrowphaseParamsHost params{};
    params.config[0] = dt;
    params.counts[0] = static_cast<std::uint32_t>(pairCount);
    params.counts[1] = static_cast<std::uint32_t>(cacheCount);
    params.counts[2] = (restCombineMode & 0xFFu) | ((fricCombineMode & 0xFFu) << 8);

    const unsigned threads = static_cast<unsigned>(pairCount);
    auto cmdBuf = cmdQueue->getAvailableBuffer();

    // Pass 1 — count.
    {
        OmegaGTE::GEComputePassDescriptor d;
        cmdBuf->startComputePass(d);
        cmdBuf->setComputePipelineState(countPso);
        cmdBuf->bindResourceAtComputeShader(bodyPosAct, kNpPosAct);
        cmdBuf->bindResourceAtComputeShader(bodyQuat, kNpQuat);
        cmdBuf->bindResourceAtComputeShader(npMaterial, kNpMaterial);
        cmdBuf->bindResourceAtComputeShader(bpShapeInfo, kNpShapeInfo);
        cmdBuf->bindResourceAtComputeShader(bpShapeParams, kNpShapeParams);
        cmdBuf->bindResourceAtComputeShader(bpShapeLocalPos, kNpShapeLocalPos);
        cmdBuf->bindResourceAtComputeShader(bpShapeLocalQuat, kNpShapeLocalQuat);
        cmdBuf->bindResourceAtComputeShader(bpHullVerts, kNpHullVerts);
        cmdBuf->bindResourceAtComputeShader(bpPairsSorted, kNpPairs);
        cmdBuf->bindResourceAtComputeShader(npContactFlag, kNpContactFlag);
        cmdBuf->bindResourceAtComputeShader(npPointCount, kNpPointCount);
        cmdBuf->bindResourceAtComputeShader(npCpuFallback, kNpCpuFallback);
        cmdBuf->setComputeConstants(&params, sizeof(params));
        cmdBuf->dispatchThreads(threads, 1, 1);
        cmdBuf->finishComputePass();
    }

    // Inclusive Hillis-Steele scans over the two count arrays, ping-ponging
    // dedicated scratch pairs so the flag scan's result survives the point
    // scan. The final buffer of each chain is bound to the build pass.
    auto scanChain = [&](SharedHandle<OmegaGTE::GEBuffer>& source,
                         SharedHandle<OmegaGTE::GEBuffer>& pingA,
                         SharedHandle<OmegaGTE::GEBuffer>& pingB)
        -> SharedHandle<OmegaGTE::GEBuffer>* {
        SharedHandle<OmegaGTE::GEBuffer>* cur = &source;
        SharedHandle<OmegaGTE::GEBuffer>* nxt = &pingA;
        for (std::size_t stride = 1; stride < pairCount; stride *= 2) {
            AQNarrowphaseParamsHost scanParams = params;
            scanParams.counts[3] = static_cast<std::uint32_t>(stride);
            OmegaGTE::GEComputePassDescriptor d;
            cmdBuf->startComputePass(d);
            cmdBuf->setComputePipelineState(scanPso);
            cmdBuf->bindResourceAtComputeShader(*cur, kNpScanIn);
            cmdBuf->bindResourceAtComputeShader(*nxt, kNpScanOut);
            cmdBuf->setComputeConstants(&scanParams, sizeof(scanParams));
            cmdBuf->dispatchThreads(threads, 1, 1);
            cmdBuf->finishComputePass();
            cur = nxt;
            nxt = (cur == &pingA) ? &pingB : &pingA;
        }
        return cur;
    };
    npScanFlagFinal = *scanChain(npContactFlag, npScanA, npScanB);
    npScanPtsFinal = *scanChain(npPointCount, npScanC, npScanD);

    // Pass 2 — build.
    {
        OmegaGTE::GEComputePassDescriptor d;
        cmdBuf->startComputePass(d);
        cmdBuf->setComputePipelineState(buildPso);
        cmdBuf->bindResourceAtComputeShader(bodyPosAct, kNpPosAct);
        cmdBuf->bindResourceAtComputeShader(bodyQuat, kNpQuat);
        cmdBuf->bindResourceAtComputeShader(bodyVelIm, kNpVelIm);
        cmdBuf->bindResourceAtComputeShader(bodyWbMax, kNpWbMax);
        cmdBuf->bindResourceAtComputeShader(bodyInvIGs, kNpInvIGs);
        cmdBuf->bindResourceAtComputeShader(bodyCom, kNpCom);
        cmdBuf->bindResourceAtComputeShader(npMaterial, kNpMaterial);
        cmdBuf->bindResourceAtComputeShader(bpShapeInfo, kNpShapeInfo);
        cmdBuf->bindResourceAtComputeShader(bpShapeParams, kNpShapeParams);
        cmdBuf->bindResourceAtComputeShader(bpShapeLocalPos, kNpShapeLocalPos);
        cmdBuf->bindResourceAtComputeShader(bpShapeLocalQuat, kNpShapeLocalQuat);
        cmdBuf->bindResourceAtComputeShader(bpHullVerts, kNpHullVerts);
        cmdBuf->bindResourceAtComputeShader(bpPairsSorted, kNpPairs);
        cmdBuf->bindResourceAtComputeShader(npScanFlagFinal, kNpScanFlagFinal);
        cmdBuf->bindResourceAtComputeShader(npScanPtsFinal, kNpScanPtsFinal);
        cmdBuf->bindResourceAtComputeShader(npCacheKeys, kNpCacheKeys);
        cmdBuf->bindResourceAtComputeShader(npCacheVals, kNpCacheVals);
        cmdBuf->bindResourceAtComputeShader(npManifolds, kNpManifolds);
        cmdBuf->bindResourceAtComputeShader(npRows, kNpRows);
        cmdBuf->setComputeConstants(&params, sizeof(params));
        cmdBuf->dispatchThreads(threads, 1, 1);
        cmdBuf->finishComputePass();
    }

    cmdQueue->submitCommandBuffer(cmdBuf);
    cmdQueue->commitToGPUAndWait();
    return true;
}

bool AQComputeBackend::downloadNarrowphase(OmegaCommon::Vector<ManifoldOut>& manifolds,
                                           OmegaCommon::Vector<RowOut>& rows,
                                           OmegaCommon::Vector<std::uint32_t>& cpuFallback,
                                           std::size_t pairCount) {
    manifolds.clear();
    rows.clear();
    cpuFallback.clear();
    if (pairCount == 0) {
        return true;
    }
    if (pairCount > npPairCapacity || !npScanFlagFinal || !npScanPtsFinal) {
        return false;
    }

    // Totals ride the LAST element of the inclusive scans.
    auto readScanTotal = [&](SharedHandle<OmegaGTE::GEBuffer>& buf) -> std::uint32_t {
        auto reader = OmegaGTE::GEBufferReader::Create();
        reader->setInputBuffer(buf);
        reader->setStructLayout({OMEGASL_UINT});
        std::uint32_t v = 0;
        for (std::size_t i = 0; i < pairCount; ++i) {
            reader->structBegin();
            reader->getUint(v);
            reader->structEnd();
        }
        return v;
    };
    const std::uint32_t manifoldCount = readScanTotal(npScanFlagFinal);
    const std::uint32_t rowCount = 3u * readScanTotal(npScanPtsFinal);

    // Per-pair CPU-fallback flags.
    {
        auto reader = OmegaGTE::GEBufferReader::Create();
        reader->setInputBuffer(npCpuFallback);
        reader->setStructLayout({OMEGASL_UINT});
        cpuFallback.resize(pairCount);
        for (std::size_t i = 0; i < pairCount; ++i) {
            reader->structBegin();
            reader->getUint(cpuFallback[i]);
            reader->structEnd();
        }
    }

    // Manifolds: uint4 + normal + materials + 4 pos + 4 accum + keys.
    if (manifoldCount > 0) {
        auto reader = OmegaGTE::GEBufferReader::Create();
        reader->setInputBuffer(npManifolds);
        reader->setStructLayout({OMEGASL_UINT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4,
                                 OMEGASL_FLOAT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4,
                                 OMEGASL_FLOAT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4,
                                 OMEGASL_UINT4});
        manifolds.resize(manifoldCount);
        for (std::uint32_t i = 0; i < manifoldCount; ++i) {
            ManifoldOut& out = manifolds[i];
            reader->structBegin();
            auto hdr = OmegaGTE::UVec<4>::Create();
            reader->getUint4(hdr);
            out.a = hdr[0][0];
            out.b = hdr[1][0];
            out.pointCount = hdr[2][0];
            auto nrm = OmegaGTE::FVec<4>::Create();
            reader->getFloat4(nrm);
            out.normal[0] = nrm[0][0]; out.normal[1] = nrm[1][0]; out.normal[2] = nrm[2][0];
            auto mats = OmegaGTE::FVec<4>::Create();
            reader->getFloat4(mats);
            out.restitutionCombined = mats[0][0];
            out.frictionCombined = mats[1][0];
            for (int k = 0; k < 4; ++k) {
                auto pp = OmegaGTE::FVec<4>::Create();
                reader->getFloat4(pp);
                out.pointPos[k][0] = pp[0][0];
                out.pointPos[k][1] = pp[1][0];
                out.pointPos[k][2] = pp[2][0];
                out.pointDepth[k] = pp[3][0];
            }
            for (int k = 0; k < 4; ++k) {
                auto pa = OmegaGTE::FVec<4>::Create();
                reader->getFloat4(pa);
                out.pointAccum[k][0] = pa[0][0];
                out.pointAccum[k][1] = pa[1][0];
                out.pointAccum[k][2] = pa[2][0];
            }
            auto keys = OmegaGTE::UVec<4>::Create();
            reader->getUint4(keys);
            for (int k = 0; k < 4; ++k) {
                out.pointKey[k] = keys[k][0];
            }
            reader->structEnd();
        }
    }

    // Rows: uint4 meta + cp + rA + rB + dir + terms + extra.
    if (rowCount > 0) {
        auto reader = OmegaGTE::GEBufferReader::Create();
        reader->setInputBuffer(npRows);
        reader->setStructLayout({OMEGASL_UINT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4,
                                 OMEGASL_FLOAT4, OMEGASL_FLOAT4, OMEGASL_FLOAT4,
                                 OMEGASL_FLOAT4});
        rows.resize(rowCount);
        for (std::uint32_t i = 0; i < rowCount; ++i) {
            RowOut& out = rows[i];
            reader->structBegin();
            auto meta = OmegaGTE::UVec<4>::Create();
            reader->getUint4(meta);
            out.kind = meta[0][0];
            out.bodyA = meta[1][0];
            out.bodyB = meta[2][0];
            out.peerRow = meta[3][0];
            auto readV3 = [&](float* dst) {
                auto v = OmegaGTE::FVec<4>::Create();
                reader->getFloat4(v);
                dst[0] = v[0][0]; dst[1] = v[1][0]; dst[2] = v[2][0];
            };
            readV3(out.contactPoint);
            readV3(out.rA);
            readV3(out.rB);
            readV3(out.direction);
            auto terms = OmegaGTE::FVec<4>::Create();
            reader->getFloat4(terms);
            out.effectiveMass = terms[0][0];
            out.bias = terms[1][0];
            out.accumImpulse = terms[2][0];
            out.frictionCoeff = terms[3][0];
            auto extra = OmegaGTE::FVec<4>::Create();
            reader->getFloat4(extra);
            out.compliance = extra[0][0];
            out.isAngular = (extra[1][0] != 0.f);
            reader->structEnd();
        }
    }
    return true;
}

// ============================================================================
// Phase 5f — colored constraint solver.
// ============================================================================

bool AQComputeBackend::setSolveGroups(const OmegaCommon::Vector<SolveGroupIn>& groups) {
    if (!gpuEngine) {
        return false;
    }
    svColorRanges.clear();
    const std::size_t n = groups.size();
    if (n == 0) {
        return true;
    }

    // Greedy coloring (plan §11.3 first cut): deterministic — group order,
    // first free color. Conflict = a shared FINITE-MASS body (the position
    // solve writes pseudo state into sleeping finite-mass bodies too).
    std::uint32_t maxBody = 0;
    for (const auto& g : groups) {
        maxBody = std::max(maxBody, std::max(g.bodyA, g.bodyB));
    }
    OmegaCommon::Vector<OmegaCommon::Vector<bool>> usedColors(maxBody + 1);
    OmegaCommon::Vector<std::uint32_t> colorOf(n, 0u);
    std::uint32_t colorCount = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const SolveGroupIn& g = groups[i];
        std::uint32_t c = 0;
        auto usedBy = [&](std::uint32_t body, std::uint32_t color) {
            const auto& u = usedColors[body];
            return color < u.size() && u[color];
        };
        while ((g.finiteA && usedBy(g.bodyA, c)) || (g.finiteB && usedBy(g.bodyB, c))) {
            ++c;
        }
        colorOf[i] = c;
        colorCount = std::max(colorCount, c + 1);
        auto mark = [&](std::uint32_t body) {
            auto& u = usedColors[body];
            if (u.size() <= c) {
                u.resize(c + 1, false);
            }
            u[c] = true;
        };
        if (g.finiteA) { mark(g.bodyA); }
        if (g.finiteB) { mark(g.bodyB); }
    }

    // §9 degenerate-coloring loud guard: colors approaching the group count
    // means near-serial dispatches — the Jacobi+mass-splitting fallback
    // (plan §11.3) is the recorded escape hatch.
    if (colorCount > 4 && colorCount * 2 > n) {
        std::cerr << "AQComputeBackend: degenerate constraint coloring — "
                  << colorCount << " colors over " << n
                  << " groups (near-serial solve; see plan §11.3)\n";
    }

    // Order groups by color (stable within a color).
    OmegaCommon::Vector<std::uint32_t> byColor;
    byColor.reserve(n);
    for (std::uint32_t c = 0; c < colorCount; ++c) {
        const std::uint32_t start = static_cast<std::uint32_t>(byColor.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (colorOf[i] == c) {
                byColor.push_back(static_cast<std::uint32_t>(i));
            }
        }
        svColorRanges.push_back({start, static_cast<std::uint32_t>(byColor.size()) - start});
    }

    // Upload the group table + by-color list.
    const std::size_t u4 = OmegaGTE::omegaSLStructStride({OMEGASL_UINT4});
    const std::size_t u1 = OmegaGTE::omegaSLStructStride({OMEGASL_UINT});
    if (n > svGroupCapacity) {
        std::size_t cap = (svGroupCapacity == 0) ? 64 : svGroupCapacity;
        while (cap < n) { cap *= 2; }
        OmegaGTE::BufferDescriptor gDesc{OmegaGTE::BufferDescriptor::Upload, cap * u4, u4};
        OmegaGTE::BufferDescriptor cDesc{OmegaGTE::BufferDescriptor::Upload, cap * u1, u1};
        svGroups = gpuEngine->makeBuffer(gDesc);
        svGroupsByColor = gpuEngine->makeBuffer(cDesc);
        if (!svGroups || !svGroupsByColor) {
            svGroupCapacity = 0;
            return false;
        }
        svGroupCapacity = cap;
    }
    {
        auto writer = OmegaGTE::GEBufferWriter::Create();
        writer->setOutputBuffer(svGroups);
        for (const auto& g : groups) {
            auto v = OmegaGTE::UVec<4>::Create();
            v[0][0] = g.firstRow;
            v[1][0] = g.rowCount;
            v[2][0] = g.manifoldIndex;
            writer->structBegin();
            writer->writeUint4(v);
            writer->structEnd();
            writer->sendToBuffer();
        }
        writer->flush();
    }
    {
        auto writer = OmegaGTE::GEBufferWriter::Create();
        writer->setOutputBuffer(svGroupsByColor);
        for (std::uint32_t gi : byColor) {
            writer->structBegin();
            writer->writeUint(gi);
            writer->structEnd();
            writer->sendToBuffer();
        }
        writer->flush();
    }
    return true;
}

bool AQComputeBackend::encodeSolve(float dt, int velocityIters, int positionIters,
                                   bool warmStart, std::size_t bodyCount) {
    if (!gpuEngine || !cmdQueue) {
        return false;
    }
    if (svColorRanges.empty()) {
        return true;   // nothing to solve
    }
    if (!npRows || !npManifolds) {
        std::cerr << "AQComputeBackend::encodeSolve: no rows/manifolds (run 5e first)\n";
        return false;
    }
    auto velPso = pipeline("AQSolveVelocityColor");
    auto posPso = pipeline("AQSolvePositionColor");
    if (!velPso || !posPso) {
        return false;
    }

    // The split-impulse accumulators start zeroed each sub-step (the CPU
    // zeroes them at the top of runNarrowphaseAndSolve). Both are Universal
    // buffers (allocated with the body pool) — the zeros stage in the backend
    // and land in the GPU primaries when the solve passes bind them.
    OmegaCommon::Vector<float> zeros(bodyCount, 0.f);
    if (!writeF4Buffer(bodyPseudo, zeros.data(), zeros.data(), zeros.data(), nullptr, bodyCount) ||
        !writeF4Buffer(bodyPseudoAng, zeros.data(), zeros.data(), zeros.data(), nullptr, bodyCount)) {
        return false;
    }

    AQSolverParamsHost params{};
    params.config[0] = dt;
    params.counts[3] = static_cast<std::uint32_t>(bodyCount);

    auto cmdBuf = cmdQueue->getAvailableBuffer();

    auto velocityColorPass = [&](std::uint32_t start, std::uint32_t count,
                                 std::uint32_t mode) {
        AQSolverParamsHost p = params;
        p.counts[0] = start;
        p.counts[1] = count;
        p.counts[2] = mode;
        OmegaGTE::GEComputePassDescriptor d;
        cmdBuf->startComputePass(d);
        cmdBuf->setComputePipelineState(velPso);
        cmdBuf->bindResourceAtComputeShader(bodyPosAct, kSvPosAct);
        cmdBuf->bindResourceAtComputeShader(bodyVelIm, kSvVelIm);
        cmdBuf->bindResourceAtComputeShader(bodyQuat, kSvQuat);
        cmdBuf->bindResourceAtComputeShader(bodyWbMax, kSvWbMax);
        cmdBuf->bindResourceAtComputeShader(bodyInvIGs, kSvInvIGs);
        cmdBuf->bindResourceAtComputeShader(npRows, kSvRows);
        cmdBuf->bindResourceAtComputeShader(svGroups, kSvGroups);
        cmdBuf->bindResourceAtComputeShader(svGroupsByColor, kSvGroupsByColor);
        cmdBuf->setComputeConstants(&p, sizeof(p));
        cmdBuf->dispatchThreads(count, 1, 1);
        cmdBuf->finishComputePass();
    };

    // Warm start (CPU pass D), then the colored PGS sweeps (pass E).
    if (warmStart) {
        for (const auto& range : svColorRanges) {
            velocityColorPass(range.first, range.second, 0u);
        }
    }
    for (int iter = 0; iter < velocityIters; ++iter) {
        for (const auto& range : svColorRanges) {
            velocityColorPass(range.first, range.second, 1u);
        }
    }

    // Split-impulse position solve (pass F).
    for (int iter = 0; iter < positionIters; ++iter) {
        for (const auto& range : svColorRanges) {
            AQSolverParamsHost p = params;
            p.counts[0] = range.first;
            p.counts[1] = range.second;
            OmegaGTE::GEComputePassDescriptor d;
            cmdBuf->startComputePass(d);
            cmdBuf->setComputePipelineState(posPso);
            cmdBuf->bindResourceAtComputeShader(bodyPosAct, kSvPosAct);
            cmdBuf->bindResourceAtComputeShader(bodyVelIm, kSvVelIm);
            cmdBuf->bindResourceAtComputeShader(bodyQuat, kSvQuat);
            cmdBuf->bindResourceAtComputeShader(bodyInvIGs, kSvInvIGs);
            cmdBuf->bindResourceAtComputeShader(bodyCom, kSvCom);
            cmdBuf->bindResourceAtComputeShader(svGroups, kSvGroups);
            cmdBuf->bindResourceAtComputeShader(svGroupsByColor, kSvGroupsByColor);
            cmdBuf->bindResourceAtComputeShader(npManifolds, kSvManifolds);
            cmdBuf->bindResourceAtComputeShader(bodyPseudo, kSvPseudoLin);
            cmdBuf->bindResourceAtComputeShader(bodyPseudoAng, kSvPseudoAng);
            cmdBuf->setComputeConstants(&p, sizeof(p));
            cmdBuf->dispatchThreads(range.second, 1, 1);
            cmdBuf->finishComputePass();
        }
    }

    cmdQueue->submitCommandBuffer(cmdBuf);
    cmdQueue->commitToGPUAndWait();
    return true;
}

bool AQComputeBackend::encodeVelocityHalfStep(float dt, const float gravity[3],
                                              std::size_t bodyCount) {
    if (!gpuEngine || !cmdQueue || bodyCount == 0) {
        return bodyCount == 0;
    }
    auto pso = pipeline("AQIntegrateVelocity");
    if (!pso) {
        return false;
    }
    AQStepParamsHost params{};
    params.gravityDt[0] = gravity[0];
    params.gravityDt[1] = gravity[1];
    params.gravityDt[2] = gravity[2];
    params.gravityDt[3] = dt;
    params.counts[0] = static_cast<std::uint32_t>(bodyCount);
    auto cmdBuf = cmdQueue->getAvailableBuffer();
    OmegaGTE::GEComputePassDescriptor d;
    cmdBuf->startComputePass(d);
    cmdBuf->setComputePipelineState(pso);
    cmdBuf->bindResourceAtComputeShader(bodyPosAct, kSlotPosAct);
    cmdBuf->bindResourceAtComputeShader(bodyVelIm, kSlotVelIm);
    cmdBuf->bindResourceAtComputeShader(bodyQuat, kSlotQuat);
    cmdBuf->bindResourceAtComputeShader(bodyWbMax, kSlotWbMax);
    cmdBuf->bindResourceAtComputeShader(bodyInvIGs, kSlotInvIGs);
    cmdBuf->bindResourceAtComputeShader(bodyForceLd, kSlotForceLd);
    cmdBuf->bindResourceAtComputeShader(bodyTorqueAd, kSlotTorqueAd);
    cmdBuf->setComputeConstants(&params, sizeof(params));
    cmdBuf->dispatchThreads(static_cast<unsigned>(bodyCount), 1, 1);
    cmdBuf->finishComputePass();
    cmdQueue->submitCommandBuffer(cmdBuf);
    cmdQueue->commitToGPUAndWait();
    return true;
}

bool AQComputeBackend::encodePositionHalfStep(float dt, const float gravity[3],
                                              std::size_t bodyCount) {
    if (!gpuEngine || !cmdQueue || bodyCount == 0) {
        return bodyCount == 0;
    }
    auto pso = pipeline("AQIntegratePosition");
    if (!pso) {
        return false;
    }
    AQStepParamsHost params{};
    params.gravityDt[0] = gravity[0];
    params.gravityDt[1] = gravity[1];
    params.gravityDt[2] = gravity[2];
    params.gravityDt[3] = dt;
    params.counts[0] = static_cast<std::uint32_t>(bodyCount);
    auto cmdBuf = cmdQueue->getAvailableBuffer();
    OmegaGTE::GEComputePassDescriptor d;
    cmdBuf->startComputePass(d);
    cmdBuf->setComputePipelineState(pso);
    cmdBuf->bindResourceAtComputeShader(bodyPosAct, kSlotPosAct);
    cmdBuf->bindResourceAtComputeShader(bodyVelIm, kSlotVelIm);
    cmdBuf->bindResourceAtComputeShader(bodyQuat, kSlotQuat);
    cmdBuf->bindResourceAtComputeShader(bodyWbMax, kSlotWbMax);
    cmdBuf->bindResourceAtComputeShader(bodyForceLd, kSlotForceLd);
    cmdBuf->bindResourceAtComputeShader(bodyTorqueAd, kSlotTorqueAd);
    cmdBuf->bindResourceAtComputeShader(bodyPseudo, kSlotPseudo);
    cmdBuf->setComputeConstants(&params, sizeof(params));
    cmdBuf->dispatchThreads(static_cast<unsigned>(bodyCount), 1, 1);
    cmdBuf->finishComputePass();
    cmdQueue->submitCommandBuffer(cmdBuf);
    cmdQueue->commitToGPUAndWait();
    return true;
}

bool AQComputeBackend::selfTest() {
    if (!gpuEngine || !cmdQueue) {
        std::cerr << "AQComputeBackend::selfTest: engine or command queue is null\n";
        return false;
    }
    if (!kernelLib) {
        std::cerr << "AQComputeBackend::selfTest: kernel library not loaded "
                     "(call loadKernelLibrary first)\n";
        return false;
    }

    auto probePipe = pipeline("AQProbeDouble");
    if (!probePipe) {
        return false;
    }

    constexpr unsigned kElementCount = 64;
    const size_t structSize = bodyBufferStride();

    OmegaGTE::BufferDescriptor inDesc{OmegaGTE::BufferDescriptor::Upload,
                                      kElementCount * structSize, structSize};
    // The kernel writes the output (`out` → UAV), so it must be Readback
    // usage — the CPU reads it back after the dispatch.
    OmegaGTE::BufferDescriptor outDesc{OmegaGTE::BufferDescriptor::Readback,
                                       kElementCount * structSize, structSize};
    auto inBuffer = gpuEngine->makeBuffer(inDesc);
    auto outBuffer = gpuEngine->makeBuffer(outDesc);
    if (!inBuffer || !outBuffer) {
        std::cerr << "AQComputeBackend::selfTest: failed to allocate probe buffers\n";
        return false;
    }

    // Fill the input with (i, 10i, 100i, 1000i).
    auto writer = OmegaGTE::GEBufferWriter::Create();
    writer->setOutputBuffer(inBuffer);
    for (unsigned i = 0; i < kElementCount; ++i) {
        auto v = OmegaGTE::FVec<4>::Create();
        v[0][0] = float(i);
        v[1][0] = float(i) * 10.f;
        v[2][0] = float(i) * 100.f;
        v[3][0] = float(i) * 1000.f;
        writer->structBegin();
        writer->writeFloat4(v);
        writer->structEnd();
        writer->sendToBuffer();
    }
    writer->flush();

    auto cmdBuf = cmdQueue->getAvailableBuffer();
    OmegaGTE::GEComputePassDescriptor passDesc;
    cmdBuf->startComputePass(passDesc);
    cmdBuf->setComputePipelineState(probePipe);
    cmdBuf->bindResourceAtComputeShader(inBuffer, 0);
    cmdBuf->bindResourceAtComputeShader(outBuffer, 1);
    cmdBuf->dispatchThreads(kElementCount, 1, 1);
    cmdBuf->finishComputePass();
    cmdQueue->submitCommandBuffer(cmdBuf);
    cmdQueue->commitToGPUAndWait();

    auto reader = OmegaGTE::GEBufferReader::Create();
    reader->setInputBuffer(outBuffer);
    reader->setStructLayout({OMEGASL_FLOAT4});
    for (unsigned i = 0; i < kElementCount; ++i) {
        reader->structBegin();
        auto v = OmegaGTE::FVec<4>::Create();
        reader->getFloat4(v);
        reader->structEnd();
        if (std::fabs(v[0][0] - float(i) * 2.f) > 1e-3f ||
            std::fabs(v[1][0] - float(i) * 20.f) > 1e-3f ||
            std::fabs(v[2][0] - float(i) * 200.f) > 1e-3f ||
            std::fabs(v[3][0] - float(i) * 2000.f) > 1e-3f) {
            std::cerr << "AQComputeBackend::selfTest: GPU output mismatch at " << i << "\n";
            return false;
        }
    }
    return true;
}
