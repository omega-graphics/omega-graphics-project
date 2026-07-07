// AQUA Phase 6 GPU sub-phase — the particle pillar's compute-backend half:
// the reusable exclusive-scan primitive (6g) and, layered on it, the resident
// particle pool + live-path encodes (6h/6i). Split out of AQComputeBackend.cpp
// (the AQSpaceParticles.cpp second-TU pattern) so the particle pillar's GPU
// half lives beside its CPU half.
//
// Scan (6g, per the 2026-07-07 recency audit Q3): multi-pass reduce-then-scan
// — per-block shared-memory scans, a recursively scanned block-sums array,
// and a uniform add pass. Deliberately NOT decoupled look-back: single-pass
// scans require a forward-progress guarantee that none of AQUA's three
// backends portably provides (it can deadlock on Metal hardware outright).
// The chain is a callable primitive over ANY uint buffer, decoupled from the
// narrowphase's inlined Hillis-Steele lambda — the debt §13.1 recorded, paid.

#include "AQComputeBackend.h"
#include "AQComputeUtil.h"

#include <aqua/AQParticles.h>   // AQForceField (POD, flattened into AQFieldGPU)

#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GECommandQueue.h>

#include <iostream>

namespace {

// MUST match compute(x=128,...) in AQScan.omegasl — the host dispatches full
// blocks so the tail block's high lanes exist on every backend.
constexpr std::size_t kAQScanBlock = 128;

// Host mirror of AQScanParams: one uint4.
struct AQScanParamsHost {
    std::uint32_t counts[4];
};

// Kernel buffer slots (AQScan.omegasl).
constexpr unsigned kScanIn   = 1;
constexpr unsigned kScanOut  = 2;
constexpr unsigned kScanSums = 3;

std::size_t scanBlocksFor(std::size_t n) {
    return (n + kAQScanBlock - 1) / kAQScanBlock;
}

// Host mirror of AQParticleParams (AQParticles.omegasl).
struct AQParticleParamsHost {
    float         config[4];
    std::uint32_t counts[4];
};

// Kernel buffer slots (AQParticles.omegasl).
constexpr unsigned kPtPosR       = 1;
constexpr unsigned kPtVelIM      = 2;
constexpr unsigned kPtDeath      = 3;
constexpr unsigned kPtFlags      = 4;
constexpr unsigned kPtFields     = 5;
constexpr unsigned kPtStagePosR  = 6;
constexpr unsigned kPtStageVelIM = 7;
constexpr unsigned kPtStageDeath = 8;
constexpr unsigned kPtOffsets    = 9;
constexpr unsigned kPtDstPosR    = 10;
constexpr unsigned kPtDstVelIM   = 11;
constexpr unsigned kPtDstDeath   = 12;
constexpr unsigned kPtColliders  = 13;

// AQFieldGPU / AQColliderGPU: uint4 + 3 x float4 (64 B, std430).
constexpr std::size_t kPtRecStride = 4 * 16;

} // namespace

bool AQComputeBackend::ensureScanCapacity(std::size_t n) {
    if (!gpuEngine) {
        return false;
    }
    if (n <= scanCapacity && scanInput) {
        return true;
    }
    std::size_t cap = (scanCapacity == 0) ? kAQScanBlock : scanCapacity;
    while (cap < n) { cap *= 2; }

    const std::size_t u1 = aqU1Stride();
    OmegaGTE::BufferDescriptor inDesc{OmegaGTE::BufferDescriptor::Upload, cap * u1, u1};
    OmegaGTE::BufferDescriptor outDesc{OmegaGTE::BufferDescriptor::Readback, cap * u1, u1};
    scanInput  = gpuEngine->makeBuffer(inDesc);
    scanOutput = gpuEngine->makeBuffer(outDesc);

    // One (sums, scannedSums) pair per recursion level until a level fits in
    // one block. All GPU-written intermediates → Readback usage.
    scanLevelSums.clear();
    scanLevelScan.clear();
    for (std::size_t levelSize = scanBlocksFor(cap); ;
         levelSize = scanBlocksFor(levelSize)) {
        OmegaGTE::BufferDescriptor lvlDesc{OmegaGTE::BufferDescriptor::Readback,
                                           levelSize * u1, u1};
        scanLevelSums.push_back(gpuEngine->makeBuffer(lvlDesc));
        scanLevelScan.push_back(gpuEngine->makeBuffer(lvlDesc));
        if (!scanLevelSums.back() || !scanLevelScan.back()) {
            std::cerr << "AQComputeBackend: scan level-buffer allocation failed\n";
            scanCapacity = 0;
            return false;
        }
        if (levelSize <= 1) {
            break;
        }
    }
    if (!scanInput || !scanOutput) {
        std::cerr << "AQComputeBackend: scan buffer allocation failed at " << cap << "\n";
        scanCapacity = 0;
        return false;
    }
    scanCapacity = cap;
    return true;
}

bool AQComputeBackend::encodeScanExclusive(
    const SharedHandle<OmegaGTE::GECommandBuffer>& cmdBuf,
    SharedHandle<OmegaGTE::GEBuffer> input,
    SharedHandle<OmegaGTE::GEBuffer> output,
    std::size_t n, unsigned level) {
    if (n == 0) {
        return true;
    }
    if (level >= scanLevelSums.size()) {
        std::cerr << "AQComputeBackend::encodeScanExclusive: recursion deeper than "
                     "the ensured level buffers (call ensureScanCapacity first)\n";
        return false;
    }
    auto blockPso = pipeline("AQScanBlockExclusive");
    auto addPso   = pipeline("AQScanAddOffsets");
    if (!blockPso || !addPso) {
        return false;
    }

    const std::size_t blocks = scanBlocksFor(n);
    const unsigned fullThreads = static_cast<unsigned>(blocks * kAQScanBlock);

    AQScanParamsHost params{};
    params.counts[0] = static_cast<std::uint32_t>(n);

    {
        OmegaGTE::GEComputePassDescriptor d;
        cmdBuf->startComputePass(d);
        cmdBuf->setComputePipelineState(blockPso);
        cmdBuf->bindResourceAtComputeShader(input, kScanIn);
        cmdBuf->bindResourceAtComputeShader(output, kScanOut);
        cmdBuf->bindResourceAtComputeShader(scanLevelSums[level], kScanSums);
        cmdBuf->setComputeConstants(&params, sizeof(params));
        cmdBuf->dispatchThreads(fullThreads, 1, 1);
        cmdBuf->finishComputePass();
    }
    if (blocks > 1) {
        // Scan the block sums (recursively — depth ≤ 3 up to 2M elements),
        // then fold the scanned offsets back into every element.
        if (!encodeScanExclusive(cmdBuf, scanLevelSums[level], scanLevelScan[level],
                                 blocks, level + 1)) {
            return false;
        }
        OmegaGTE::GEComputePassDescriptor d;
        cmdBuf->startComputePass(d);
        cmdBuf->setComputePipelineState(addPso);
        cmdBuf->bindResourceAtComputeShader(output, kScanOut);
        cmdBuf->bindResourceAtComputeShader(scanLevelScan[level], kScanSums);
        cmdBuf->setComputeConstants(&params, sizeof(params));
        cmdBuf->dispatchThreads(fullThreads, 1, 1);
        cmdBuf->finishComputePass();
    }
    return true;
}

bool AQComputeBackend::scanExclusive(const OmegaCommon::Vector<std::uint32_t>& in,
                                     OmegaCommon::Vector<std::uint32_t>& out) {
    const std::size_t n = in.size();
    out.clear();
    if (n == 0) {
        return true;
    }
    if (!gpuEngine || !cmdQueue) {
        std::cerr << "AQComputeBackend::scanExclusive: engine or queue is null\n";
        return false;
    }
    if (!ensureScanCapacity(n)) {
        return false;
    }
    if (!aqWriteU1Buffer(scanInput, in.data(), n)) {
        return false;
    }

    auto cmdBuf = cmdQueue->getAvailableBuffer();
    if (!encodeScanExclusive(cmdBuf, scanInput, scanOutput, n, 0)) {
        return false;
    }
    cmdQueue->submitCommandBuffer(cmdBuf);
    cmdQueue->commitToGPUAndWait();

    out.resize(n);
    return aqReadU1Buffer(scanOutput, out.data(), n);
}

// ============================================================================
// Phase 6h/6i — resident particle pools + the per-advance frame encode.
// ============================================================================

AQComputeBackend::ParticlePoolGPU* AQComputeBackend::ptPoolAt(std::uint64_t id) {
    auto it = ptPools.find(id);
    return (it == ptPools.end()) ? nullptr : &it->second;
}

bool AQComputeBackend::particlesEnsurePool(std::uint64_t id, std::uint32_t capacity) {
    if (!gpuEngine || !cmdQueue || id == 0) {
        return false;
    }
    ParticlePoolGPU& pool = ptPools[id];
    if (capacity <= pool.capacity && pool.flags) {
        return true;
    }

    const std::size_t f4 = aqF4Stride();
    const std::size_t u1 = aqU1Stride();
    const std::size_t cap = (capacity == 0) ? 1 : capacity;
    // Everything resident is GPU-read-write with a CPU read path → Readback;
    // the CPU never writes the pool directly (emission goes through the
    // Upload staging buffers + the inject kernel).
    OmegaGTE::BufferDescriptor f4Desc{OmegaGTE::BufferDescriptor::Readback, cap * f4, f4};
    OmegaGTE::BufferDescriptor u1Desc{OmegaGTE::BufferDescriptor::Readback, cap * u1, u1};
    for (int p = 0; p < 2; ++p) {
        pool.posR[p]  = gpuEngine->makeBuffer(f4Desc);
        pool.velIM[p] = gpuEngine->makeBuffer(f4Desc);
        pool.death[p] = gpuEngine->makeBuffer(u1Desc);
        if (!pool.posR[p] || !pool.velIM[p] || !pool.death[p]) {
            std::cerr << "AQComputeBackend: particle pool allocation failed at "
                      << cap << "\n";
            ptPools.erase(id);
            return false;
        }
    }
    pool.flags   = gpuEngine->makeBuffer(u1Desc);
    pool.offsets = gpuEngine->makeBuffer(u1Desc);
    if (!pool.flags || !pool.offsets) {
        std::cerr << "AQComputeBackend: particle pool allocation failed at " << cap << "\n";
        ptPools.erase(id);
        return false;
    }
    pool.capacity = cap;
    pool.cur = 0;

    // Device-zero the fresh flags over the FULL capacity (the flags buffer is
    // Readback usage — GPU-written only — so the zero is a kernel pass, not a
    // host write): AQParticleRebuildFlags with newLiveCount = 0.
    auto rebuildPso = pipeline("AQParticleRebuildFlags");
    if (!rebuildPso) {
        return false;
    }
    AQParticleParamsHost params{};
    params.counts[0] = static_cast<std::uint32_t>(cap);
    params.counts[2] = 0u;
    auto cmdBuf = cmdQueue->getAvailableBuffer();
    OmegaGTE::GEComputePassDescriptor d;
    cmdBuf->startComputePass(d);
    cmdBuf->setComputePipelineState(rebuildPso);
    cmdBuf->bindResourceAtComputeShader(pool.flags, kPtFlags);
    cmdBuf->setComputeConstants(&params, sizeof(params));
    cmdBuf->dispatchThreads(static_cast<unsigned>(cap), 1, 1);
    cmdBuf->finishComputePass();
    cmdQueue->submitCommandBuffer(cmdBuf);
    cmdQueue->commitToGPUAndWait();
    return true;
}

void AQComputeBackend::particlesReleasePool(std::uint64_t id) {
    ptPools.erase(id);
}

bool AQComputeBackend::particlesUploadStaging(std::uint64_t id,
                                              const float* posX, const float* posY, const float* posZ,
                                              const float* radius,
                                              const float* velX, const float* velY, const float* velZ,
                                              const float* invMass,
                                              const std::uint32_t* death, std::size_t count) {
    ParticlePoolGPU* pool = ptPoolAt(id);
    if (!pool) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    if (count > pool->stageCapacity || !pool->stagePosR) {
        std::size_t cap = (pool->stageCapacity == 0) ? 64 : pool->stageCapacity;
        while (cap < count) { cap *= 2; }
        const std::size_t f4 = aqF4Stride();
        const std::size_t u1 = aqU1Stride();
        OmegaGTE::BufferDescriptor f4Desc{OmegaGTE::BufferDescriptor::Upload, cap * f4, f4};
        OmegaGTE::BufferDescriptor u1Desc{OmegaGTE::BufferDescriptor::Upload, cap * u1, u1};
        pool->stagePosR  = gpuEngine->makeBuffer(f4Desc);
        pool->stageVelIM = gpuEngine->makeBuffer(f4Desc);
        pool->stageDeath = gpuEngine->makeBuffer(u1Desc);
        if (!pool->stagePosR || !pool->stageVelIM || !pool->stageDeath) {
            pool->stageCapacity = 0;
            return false;
        }
        pool->stageCapacity = cap;
    }
    return aqWriteF4Buffer(pool->stagePosR, posX, posY, posZ, radius, count) &&
           aqWriteF4Buffer(pool->stageVelIM, velX, velY, velZ, invMass, count) &&
           aqWriteU1Buffer(pool->stageDeath, death, count);
}

bool AQComputeBackend::particlesUploadFields(std::uint64_t id,
                                             const OmegaCommon::Vector<AQForceField>& fields) {
    ParticlePoolGPU* pool = ptPoolAt(id);
    if (!pool) {
        return false;
    }
    const std::size_t n = fields.size();
    if (n == 0) {
        // The integrate pass binds the buffer even with fieldCount 0 — keep a
        // one-record placeholder so the binding is always valid.
        if (!pool->fields) {
            OmegaGTE::BufferDescriptor desc{OmegaGTE::BufferDescriptor::Upload,
                                            kPtRecStride, kPtRecStride};
            pool->fields = gpuEngine->makeBuffer(desc);
            pool->fieldCapacity = 1;
        }
        return static_cast<bool>(pool->fields);
    }
    if (n > pool->fieldCapacity || !pool->fields) {
        std::size_t cap = (pool->fieldCapacity == 0) ? 4 : pool->fieldCapacity;
        while (cap < n) { cap *= 2; }
        OmegaGTE::BufferDescriptor desc{OmegaGTE::BufferDescriptor::Upload,
                                        cap * kPtRecStride, kPtRecStride};
        pool->fields = gpuEngine->makeBuffer(desc);
        if (!pool->fields) {
            pool->fieldCapacity = 0;
            return false;
        }
        pool->fieldCapacity = cap;
    }

    // Flatten AQForceField → AQFieldGPU: meta = (kind, enabled); position.w =
    // radiusOfInfluence; axis.w = the union's first param; extra.x = falloff.
    auto writer = OmegaGTE::GEBufferWriter::Create();
    writer->setOutputBuffer(pool->fields);
    for (const AQForceField& f : fields) {
        float p0 = 0.f, p1 = 0.f;
        switch (f.kind) {
        case AQFieldGravity: p0 = f.p.gravity.g; break;
        case AQFieldDrag:    p0 = f.p.drag.k; break;
        case AQFieldWind:    p0 = f.p.wind.speed; break;
        case AQFieldVortex:  p0 = f.p.vortex.strength; p1 = f.p.vortex.falloff; break;
        case AQFieldPoint:   p0 = f.p.point.strength;  p1 = f.p.point.falloff;  break;
        }
        auto meta = OmegaGTE::UVec<4>::Create();
        meta[0][0] = static_cast<std::uint32_t>(f.kind);
        meta[1][0] = f.enabled;
        auto position = OmegaGTE::FVec<4>::Create();
        position[0][0] = f.position[0][0];
        position[1][0] = f.position[1][0];
        position[2][0] = f.position[2][0];
        position[3][0] = f.radiusOfInfluence;
        auto axis = OmegaGTE::FVec<4>::Create();
        axis[0][0] = f.axis[0][0];
        axis[1][0] = f.axis[1][0];
        axis[2][0] = f.axis[2][0];
        axis[3][0] = p0;
        auto extra = OmegaGTE::FVec<4>::Create();
        extra[0][0] = p1;
        writer->structBegin();
        writer->writeUint4(meta);
        writer->writeFloat4(position);
        writer->writeFloat4(axis);
        writer->writeFloat4(extra);
        writer->structEnd();
        writer->sendToBuffer();
    }
    writer->flush();
    return true;
}

bool AQComputeBackend::particlesUploadColliders(
    std::uint64_t id, const OmegaCommon::Vector<ParticleColliderIn>& colliders) {
    ParticlePoolGPU* pool = ptPoolAt(id);
    if (!pool) {
        return false;
    }
    const std::size_t n = colliders.size();
    const std::size_t want = (n == 0) ? 1 : n;   // keep the binding valid
    if (want > pool->colliderCapacity || !pool->colliders) {
        std::size_t cap = (pool->colliderCapacity == 0) ? 4 : pool->colliderCapacity;
        while (cap < want) { cap *= 2; }
        OmegaGTE::BufferDescriptor desc{OmegaGTE::BufferDescriptor::Upload,
                                        cap * kPtRecStride, kPtRecStride};
        pool->colliders = gpuEngine->makeBuffer(desc);
        if (!pool->colliders) {
            pool->colliderCapacity = 0;
            return false;
        }
        pool->colliderCapacity = cap;
    }
    if (n == 0) {
        return true;
    }
    auto writer = OmegaGTE::GEBufferWriter::Create();
    writer->setOutputBuffer(pool->colliders);
    for (const ParticleColliderIn& c : colliders) {
        auto meta = OmegaGTE::UVec<4>::Create();
        meta[0][0] = c.shapeType;
        auto pos = OmegaGTE::FVec<4>::Create();
        pos[0][0] = c.px; pos[1][0] = c.py; pos[2][0] = c.pz; pos[3][0] = c.restitution;
        auto quat = OmegaGTE::FVec<4>::Create();
        quat[0][0] = c.qx; quat[1][0] = c.qy; quat[2][0] = c.qz; quat[3][0] = c.qw;
        auto params = OmegaGTE::FVec<4>::Create();
        params[0][0] = c.params[0]; params[1][0] = c.params[1];
        params[2][0] = c.params[2]; params[3][0] = c.params[3];
        writer->structBegin();
        writer->writeUint4(meta);
        writer->writeFloat4(pos);
        writer->writeFloat4(quat);
        writer->writeFloat4(params);
        writer->structEnd();
        writer->sendToBuffer();
    }
    writer->flush();
    return true;
}

bool AQComputeBackend::particlesEncodeFrame(const ParticleFrameDesc& desc) {
    if (!gpuEngine || !cmdQueue) {
        return false;
    }
    ParticlePoolGPU* pool = ptPoolAt(desc.systemId);
    if (!pool) {
        std::cerr << "AQComputeBackend::particlesEncodeFrame: no pool for system "
                  << desc.systemId << " (call particlesEnsurePool first)\n";
        return false;
    }
    if (desc.activeSpan > pool->capacity ||
        static_cast<std::size_t>(desc.injectStart) + desc.injectCount > pool->capacity) {
        std::cerr << "AQComputeBackend::particlesEncodeFrame: span/inject exceeds pool "
                  << "capacity " << pool->capacity << "\n";
        return false;
    }
    if (desc.activeSpan == 0) {
        return true;                       // nothing occupied, nothing to run
    }
    if (desc.injectCount > 0 && !pool->stagePosR) {
        std::cerr << "AQComputeBackend::particlesEncodeFrame: inject requested but no "
                     "staging uploaded\n";
        return false;
    }
    if (!pool->fields) {
        // Ensure a valid fields binding even when no field was ever uploaded.
        OmegaCommon::Vector<AQForceField> none;
        if (!particlesUploadFields(desc.systemId, none)) {
            return false;
        }
    }
    if (desc.colliderCount > 0 && !pool->colliders) {
        std::cerr << "AQComputeBackend::particlesEncodeFrame: collide requested but no "
                     "colliders uploaded\n";
        return false;
    }

    auto injectPso  = pipeline("AQParticleInject");
    auto integPso   = pipeline("AQParticleIntegrate");
    auto agePso     = pipeline("AQParticleAge");
    auto scatterPso = pipeline("AQParticleScatter");
    auto rebuildPso = pipeline("AQParticleRebuildFlags");
    if (!injectPso || !integPso || !agePso || !scatterPso || !rebuildPso) {
        return false;
    }
    auto collidePso = SharedHandle<OmegaGTE::GEComputePipelineState>();
    if (desc.colliderCount > 0) {
        collidePso = pipeline("AQParticleCollide");
        if (!collidePso) {
            return false;
        }
    }
    if (!ensureScanCapacity(desc.activeSpan)) {
        return false;
    }

    const int cur = pool->cur;
    const int nxt = 1 - cur;
    const unsigned spanThreads = static_cast<unsigned>(desc.activeSpan);

    AQParticleParamsHost base{};
    base.config[0] = desc.dt;
    base.counts[0] = desc.activeSpan;
    base.counts[1] = desc.fieldCount;

    auto cmdBuf = cmdQueue->getAvailableBuffer();

    // Inject — staged new particles → the pool tail.
    if (desc.injectCount > 0) {
        AQParticleParamsHost p = base;
        p.counts[2] = desc.injectStart;
        p.counts[3] = desc.injectCount;
        OmegaGTE::GEComputePassDescriptor d;
        cmdBuf->startComputePass(d);
        cmdBuf->setComputePipelineState(injectPso);
        cmdBuf->bindResourceAtComputeShader(pool->stagePosR, kPtStagePosR);
        cmdBuf->bindResourceAtComputeShader(pool->stageVelIM, kPtStageVelIM);
        cmdBuf->bindResourceAtComputeShader(pool->stageDeath, kPtStageDeath);
        cmdBuf->bindResourceAtComputeShader(pool->posR[cur], kPtPosR);
        cmdBuf->bindResourceAtComputeShader(pool->velIM[cur], kPtVelIM);
        cmdBuf->bindResourceAtComputeShader(pool->death[cur], kPtDeath);
        cmdBuf->bindResourceAtComputeShader(pool->flags, kPtFlags);
        cmdBuf->setComputeConstants(&p, sizeof(p));
        cmdBuf->dispatchThreads(static_cast<unsigned>(desc.injectCount), 1, 1);
        cmdBuf->finishComputePass();
    }

    // The sub-step loop: integrate [+ collide] + age, all per-lane.
    for (std::uint32_t s = 0; s < desc.substeps; ++s) {
        {
            OmegaGTE::GEComputePassDescriptor d;
            cmdBuf->startComputePass(d);
            cmdBuf->setComputePipelineState(integPso);
            cmdBuf->bindResourceAtComputeShader(pool->posR[cur], kPtPosR);
            cmdBuf->bindResourceAtComputeShader(pool->velIM[cur], kPtVelIM);
            cmdBuf->bindResourceAtComputeShader(pool->flags, kPtFlags);
            cmdBuf->bindResourceAtComputeShader(pool->fields, kPtFields);
            cmdBuf->setComputeConstants(&base, sizeof(base));
            cmdBuf->dispatchThreads(spanThreads, 1, 1);
            cmdBuf->finishComputePass();
        }
        if (desc.colliderCount > 0) {
            AQParticleParamsHost p = base;
            p.counts[3] = desc.colliderCount;
            OmegaGTE::GEComputePassDescriptor d;
            cmdBuf->startComputePass(d);
            cmdBuf->setComputePipelineState(collidePso);
            cmdBuf->bindResourceAtComputeShader(pool->posR[cur], kPtPosR);
            cmdBuf->bindResourceAtComputeShader(pool->velIM[cur], kPtVelIM);
            cmdBuf->bindResourceAtComputeShader(pool->flags, kPtFlags);
            cmdBuf->bindResourceAtComputeShader(pool->colliders, kPtColliders);
            cmdBuf->setComputeConstants(&p, sizeof(p));
            cmdBuf->dispatchThreads(spanThreads, 1, 1);
            cmdBuf->finishComputePass();
        }
        {
            OmegaGTE::GEComputePassDescriptor d;
            cmdBuf->startComputePass(d);
            cmdBuf->setComputePipelineState(agePso);
            cmdBuf->bindResourceAtComputeShader(pool->death[cur], kPtDeath);
            cmdBuf->bindResourceAtComputeShader(pool->flags, kPtFlags);
            cmdBuf->setComputeConstants(&base, sizeof(base));
            cmdBuf->dispatchThreads(spanThreads, 1, 1);
            cmdBuf->finishComputePass();
        }
    }

    // Stable compaction: 6g exclusive scan over the alive flags → scatter
    // into the ping-pong destination → flag rebuild from the host census.
    if (!encodeScanExclusive(cmdBuf, pool->flags, pool->offsets, desc.activeSpan, 0)) {
        return false;
    }
    {
        AQParticleParamsHost p = base;
        p.counts[2] = desc.newLiveCount;
        OmegaGTE::GEComputePassDescriptor d;
        cmdBuf->startComputePass(d);
        cmdBuf->setComputePipelineState(scatterPso);
        cmdBuf->bindResourceAtComputeShader(pool->posR[cur], kPtPosR);
        cmdBuf->bindResourceAtComputeShader(pool->velIM[cur], kPtVelIM);
        cmdBuf->bindResourceAtComputeShader(pool->death[cur], kPtDeath);
        cmdBuf->bindResourceAtComputeShader(pool->flags, kPtFlags);
        cmdBuf->bindResourceAtComputeShader(pool->offsets, kPtOffsets);
        cmdBuf->bindResourceAtComputeShader(pool->posR[nxt], kPtDstPosR);
        cmdBuf->bindResourceAtComputeShader(pool->velIM[nxt], kPtDstVelIM);
        cmdBuf->bindResourceAtComputeShader(pool->death[nxt], kPtDstDeath);
        cmdBuf->setComputeConstants(&p, sizeof(p));
        cmdBuf->dispatchThreads(spanThreads, 1, 1);
        cmdBuf->finishComputePass();
    }
    {
        AQParticleParamsHost p = base;
        p.counts[2] = desc.newLiveCount;
        OmegaGTE::GEComputePassDescriptor d;
        cmdBuf->startComputePass(d);
        cmdBuf->setComputePipelineState(rebuildPso);
        cmdBuf->bindResourceAtComputeShader(pool->flags, kPtFlags);
        cmdBuf->setComputeConstants(&p, sizeof(p));
        cmdBuf->dispatchThreads(spanThreads, 1, 1);
        cmdBuf->finishComputePass();
    }

    cmdQueue->submitCommandBuffer(cmdBuf);
    cmdQueue->commitToGPUAndWait();
    pool->cur = nxt;                       // the packed prefix is now current
    return true;
}

bool AQComputeBackend::particlesDownloadState(std::uint64_t id,
                                              float* posX, float* posY, float* posZ,
                                              float* velX, float* velY, float* velZ,
                                              std::size_t count) {
    ParticlePoolGPU* pool = ptPoolAt(id);
    if (!pool) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    if (count > pool->capacity) {
        return false;
    }
    return aqReadF4Buffer(pool->posR[pool->cur], posX, posY, posZ, nullptr, count) &&
           aqReadF4Buffer(pool->velIM[pool->cur], velX, velY, velZ, nullptr, count);
}
