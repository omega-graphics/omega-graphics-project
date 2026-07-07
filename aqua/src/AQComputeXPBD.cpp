// AQUA Phase 7f — the XPBD GPU stage: per-body buffer residency, topology
// upload, and the frame encode that drives src/kernels/AQXPBD.omegasl. Split
// out of AQComputeBackend.cpp (the AQSpaceParticles.cpp second-TU pattern) so
// the deformable pillar's GPU half lives beside its CPU half rather than
// growing the 1.5k-line rigid backend file.
//
// Dispatch shape (brief §13.4 7f): per engine sub-step of dt, `substeps`
// slices of h = dt/substeps; each slice encodes predict → λ-reset →
// iterations × (one dispatch per color, colors serial) → derive. The LIVE
// path (post-6h flip) batches a whole advance-frame — `engineSubsteps`
// back-to-back engine sub-steps — into ONE command buffer with ONE sync;
// buffers stay resident across slices, sub-steps, and frames, and the
// constraint array uploads on topology change only (the §14.1 residency
// discipline). Pools are keyed per body id (the 6h particle-pool idiom) so
// multiple XPBD bodies coexist on the device.
//
// The color ranges are contiguous slices of the color-sorted constraint
// array (AQXPBDBody::distanceSorted / batches — the layout was chosen in 7b
// precisely so this port needs no indirection table). The CPU solve visits
// constraints in the exact order these dispatches do: colors ascending,
// authoring order within a color.

#include "AQComputeBackend.h"
#include "AQComputeUtil.h"

#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GECommandQueue.h>

#include <iostream>
#include <utility>

namespace {

// Host mirror of AQXPBDStepParams (AQXPBD.omegasl): two float4s + one uint4.
struct AQXPBDStepParamsHost {
    float         gravityH[4];
    float         damping[4];
    std::uint32_t counts[4];
};

// Kernel buffer slots (AQXPBD.omegasl).
constexpr unsigned kXpPos   = 1;
constexpr unsigned kXpPrev  = 2;
constexpr unsigned kXpVel   = 3;
constexpr unsigned kXpCon   = 4;
constexpr unsigned kXpTrips = 5;

// Constraint record stride: uint4 + float4 (32 B, std430).
constexpr std::size_t kXpConStride = 2 * 16;

} // namespace

AQComputeBackend::XPBDPoolGPU* AQComputeBackend::xpbdPoolAt(std::uint64_t id) {
    auto it = xpbdPools.find(id);
    return (it == xpbdPools.end()) ? nullptr : &it->second;
}

void AQComputeBackend::xpbdReleasePool(std::uint64_t id) {
    xpbdPools.erase(id);
}

bool AQComputeBackend::ensureXPBDParticleCapacity(XPBDPoolGPU& pool,
                                                  std::size_t particleCount) {
    if (!gpuEngine) {
        return false;
    }
    if (particleCount <= pool.particleCapacity && pool.pos) {
        return true;
    }
    std::size_t cap = (pool.particleCapacity == 0) ? 64 : pool.particleCapacity;
    while (cap < particleCount) { cap *= 2; }

    const std::size_t f4 = aqF4Stride();
    // pos/vel are CPU-seeded, kernel-mutated, CPU-read-back → Universal (the
    // body-pool precedent). prev is written only by kernels → Readback.
    OmegaGTE::BufferDescriptor uniDesc{OmegaGTE::BufferDescriptor::Universal, cap * f4, f4};
    OmegaGTE::BufferDescriptor gpuDesc{OmegaGTE::BufferDescriptor::Readback, cap * f4, f4};
    pool.pos  = gpuEngine->makeBuffer(uniDesc);
    pool.vel  = gpuEngine->makeBuffer(uniDesc);
    pool.prev = gpuEngine->makeBuffer(gpuDesc);
    if (!pool.pos || !pool.vel || !pool.prev) {
        std::cerr << "AQComputeBackend: XPBD particle-buffer allocation failed at capacity "
                  << cap << "\n";
        pool.particleCapacity = 0;
        return false;
    }
    pool.particleCapacity = cap;
    return true;
}

bool AQComputeBackend::ensureXPBDConstraintCapacity(XPBDPoolGPU& pool,
                                                    std::size_t constraintCount) {
    if (!gpuEngine) {
        return false;
    }
    if (constraintCount <= pool.constraintCapacity && pool.con) {
        return true;
    }
    std::size_t cap = (pool.constraintCapacity == 0) ? 64 : pool.constraintCapacity;
    while (cap < constraintCount) { cap *= 2; }

    const std::size_t u1 = aqU1Stride();
    // λ is GPU-mutated inside the CPU-uploaded record, and the trip counters
    // are CPU-zeroed + GPU-incremented + CPU-read → both Universal.
    OmegaGTE::BufferDescriptor conDesc{OmegaGTE::BufferDescriptor::Universal,
                                       cap * kXpConStride, kXpConStride};
    OmegaGTE::BufferDescriptor tripDesc{OmegaGTE::BufferDescriptor::Universal, cap * u1, u1};
    pool.con   = gpuEngine->makeBuffer(conDesc);
    pool.trips = gpuEngine->makeBuffer(tripDesc);
    if (!pool.con || !pool.trips) {
        std::cerr << "AQComputeBackend: XPBD constraint-buffer allocation failed at capacity "
                  << cap << "\n";
        pool.constraintCapacity = 0;
        return false;
    }
    pool.constraintCapacity = cap;
    return true;
}

bool AQComputeBackend::uploadXPBDParticles(std::uint64_t id,
                                           const OmegaCommon::Vector<float>& posX,
                                           const OmegaCommon::Vector<float>& posY,
                                           const OmegaCommon::Vector<float>& posZ,
                                           const OmegaCommon::Vector<float>& invMass,
                                           const OmegaCommon::Vector<float>& velX,
                                           const OmegaCommon::Vector<float>& velY,
                                           const OmegaCommon::Vector<float>& velZ) {
    if (id == 0) {
        return false;
    }
    XPBDPoolGPU& pool = xpbdPools[id];
    const std::size_t n = posX.size();
    if (!ensureXPBDParticleCapacity(pool, n)) {
        return false;
    }
    if (n == 0) {
        return true;
    }
    // x_prev needs no host seed: AQXPBDPredict refreshes it from x at the top
    // of the first slice before anything reads it.
    return aqWriteF4Buffer(pool.pos, posX.data(), posY.data(), posZ.data(), invMass.data(), n) &&
           aqWriteF4Buffer(pool.vel, velX.data(), velY.data(), velZ.data(), nullptr, n);
}

bool AQComputeBackend::uploadXPBDConstraints(
    std::uint64_t id,
    const OmegaCommon::Vector<XPBDConstraintIn>& sorted,
    const OmegaCommon::Vector<std::pair<std::uint32_t, std::uint32_t>>& colorRanges) {
    if (id == 0) {
        return false;
    }
    XPBDPoolGPU& pool = xpbdPools[id];
    const std::size_t n = sorted.size();
    if (!ensureXPBDConstraintCapacity(pool, n)) {
        return false;
    }
    pool.colorRanges = colorRanges;
    pool.constraintCount = n;
    if (n == 0) {
        return true;
    }

    {
        auto writer = OmegaGTE::GEBufferWriter::Create();
        writer->setOutputBuffer(pool.con);
        for (const XPBDConstraintIn& c : sorted) {
            auto ab = OmegaGTE::UVec<4>::Create();
            ab[0][0] = c.a;
            ab[1][0] = c.b;
            ab[2][0] = c.color;
            auto terms = OmegaGTE::FVec<4>::Create();
            terms[0][0] = c.restLength;
            terms[1][0] = c.compliance;
            terms[2][0] = 0.f;                       // λ starts 0 on-device
            writer->structBegin();
            writer->writeUint4(ab);
            writer->writeFloat4(terms);
            writer->structEnd();
            writer->sendToBuffer();
        }
        writer->flush();
    }

    // Fresh topology ⇒ fresh guard-trip counters.
    OmegaCommon::Vector<std::uint32_t> zeros(n, 0u);
    return aqWriteU1Buffer(pool.trips, zeros.data(), n);
}

bool AQComputeBackend::encodeXPBDAdvance(std::uint64_t id, float dt, const float gravity[3],
                                         std::uint32_t substeps, std::uint32_t iterations,
                                         float velocityDamping, float explosionThreshold,
                                         std::size_t particleCount,
                                         std::uint32_t engineSubsteps) {
    if (!gpuEngine || !cmdQueue) {
        std::cerr << "AQComputeBackend::encodeXPBDAdvance: engine or queue is null\n";
        return false;
    }
    if (particleCount == 0 || engineSubsteps == 0) {
        return true;
    }
    XPBDPoolGPU* pool = xpbdPoolAt(id);
    if (!pool || particleCount > pool->particleCapacity) {
        std::cerr << "AQComputeBackend::encodeXPBDAdvance: no pool / " << particleCount
                  << " particles not uploaded for body " << id << "\n";
        return false;
    }

    auto predictPso = pipeline("AQXPBDPredict");
    auto zeroPso    = pipeline("AQXPBDZeroLambda");
    auto projPso    = pipeline("AQXPBDProjectDistance");
    auto derivePso  = pipeline("AQXPBDDerive");
    if (!predictPso || !zeroPso || !projPso || !derivePso) {
        return false;
    }

    const std::uint32_t nSlices = (substeps > 0u) ? substeps : 1u;
    const std::uint32_t nIters  = (iterations > 0u) ? iterations : 1u;
    const float h = dt / static_cast<float>(nSlices);

    AQXPBDStepParamsHost params{};
    params.gravityH[0] = gravity[0];
    params.gravityH[1] = gravity[1];
    params.gravityH[2] = gravity[2];
    params.gravityH[3] = h;
    params.damping[0] = velocityDamping;
    params.damping[1] = explosionThreshold;
    params.counts[0] = static_cast<std::uint32_t>(particleCount);
    params.counts[1] = static_cast<std::uint32_t>(pool->constraintCount);

    const unsigned particleThreads = static_cast<unsigned>(particleCount);
    const unsigned constraintThreads = static_cast<unsigned>(pool->constraintCount);

    // The whole frame (engineSubsteps × nSlices slices) rides ONE command
    // buffer; sequential compute passes give the slice ordering (predict
    // before projection before derive, colors serial) and one commit + wait
    // at the end is the per-advance sync point.
    auto cmdBuf = cmdQueue->getAvailableBuffer();
    const std::uint32_t totalSlices = engineSubsteps * nSlices;
    for (std::uint32_t s = 0; s < totalSlices; ++s) {
        {   // 1. predict
            OmegaGTE::GEComputePassDescriptor d;
            cmdBuf->startComputePass(d);
            cmdBuf->setComputePipelineState(predictPso);
            cmdBuf->bindResourceAtComputeShader(pool->pos, kXpPos);
            cmdBuf->bindResourceAtComputeShader(pool->prev, kXpPrev);
            cmdBuf->bindResourceAtComputeShader(pool->vel, kXpVel);
            cmdBuf->setComputeConstants(&params, sizeof(params));
            cmdBuf->dispatchThreads(particleThreads, 1, 1);
            cmdBuf->finishComputePass();
        }
        if (pool->constraintCount > 0) {
            // λ resets each slice, then accumulates across the slice's
            // iterations (brief §6 — the compliance oracle depends on it).
            OmegaGTE::GEComputePassDescriptor d;
            cmdBuf->startComputePass(d);
            cmdBuf->setComputePipelineState(zeroPso);
            cmdBuf->bindResourceAtComputeShader(pool->con, kXpCon);
            cmdBuf->setComputeConstants(&params, sizeof(params));
            cmdBuf->dispatchThreads(constraintThreads, 1, 1);
            cmdBuf->finishComputePass();

            // 2. project — colors serial, one conflict-free dispatch each.
            for (std::uint32_t iter = 0; iter < nIters; ++iter) {
                for (const auto& range : pool->colorRanges) {
                    if (range.second == 0) {
                        continue;
                    }
                    AQXPBDStepParamsHost p = params;
                    p.counts[2] = range.first;
                    p.counts[3] = range.second;
                    OmegaGTE::GEComputePassDescriptor pd;
                    cmdBuf->startComputePass(pd);
                    cmdBuf->setComputePipelineState(projPso);
                    cmdBuf->bindResourceAtComputeShader(pool->pos, kXpPos);
                    cmdBuf->bindResourceAtComputeShader(pool->con, kXpCon);
                    cmdBuf->bindResourceAtComputeShader(pool->trips, kXpTrips);
                    cmdBuf->setComputeConstants(&p, sizeof(p));
                    cmdBuf->dispatchThreads(range.second, 1, 1);
                    cmdBuf->finishComputePass();
                }
            }
        }
        {   // 3. derive
            OmegaGTE::GEComputePassDescriptor d;
            cmdBuf->startComputePass(d);
            cmdBuf->setComputePipelineState(derivePso);
            cmdBuf->bindResourceAtComputeShader(pool->pos, kXpPos);
            cmdBuf->bindResourceAtComputeShader(pool->prev, kXpPrev);
            cmdBuf->bindResourceAtComputeShader(pool->vel, kXpVel);
            cmdBuf->setComputeConstants(&params, sizeof(params));
            cmdBuf->dispatchThreads(particleThreads, 1, 1);
            cmdBuf->finishComputePass();
        }
    }
    cmdQueue->submitCommandBuffer(cmdBuf);
    cmdQueue->commitToGPUAndWait();
    return true;
}

bool AQComputeBackend::downloadXPBDParticles(std::uint64_t id,
                                             OmegaCommon::Vector<float>& posX,
                                             OmegaCommon::Vector<float>& posY,
                                             OmegaCommon::Vector<float>& posZ,
                                             OmegaCommon::Vector<float>& velX,
                                             OmegaCommon::Vector<float>& velY,
                                             OmegaCommon::Vector<float>& velZ,
                                             std::size_t particleCount) {
    if (particleCount == 0) {
        return true;
    }
    XPBDPoolGPU* pool = xpbdPoolAt(id);
    if (!pool || particleCount > pool->particleCapacity) {
        std::cerr << "AQComputeBackend::downloadXPBDParticles: nothing uploaded for body "
                  << id << "\n";
        return false;
    }
    posX.resize(particleCount); posY.resize(particleCount); posZ.resize(particleCount);
    velX.resize(particleCount); velY.resize(particleCount); velZ.resize(particleCount);
    return aqReadF4Buffer(pool->pos, posX.data(), posY.data(), posZ.data(), nullptr, particleCount) &&
           aqReadF4Buffer(pool->vel, velX.data(), velY.data(), velZ.data(), nullptr, particleCount);
}

bool AQComputeBackend::downloadXPBDTrips(std::uint64_t id,
                                         OmegaCommon::Vector<std::uint32_t>& trips) {
    trips.clear();
    XPBDPoolGPU* pool = xpbdPoolAt(id);
    if (!pool) {
        return false;
    }
    if (pool->constraintCount == 0) {
        return true;
    }
    trips.resize(pool->constraintCount);
    return aqReadU1Buffer(pool->trips, trips.data(), pool->constraintCount);
}
