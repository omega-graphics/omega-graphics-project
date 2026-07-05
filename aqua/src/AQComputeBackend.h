#ifndef AQUA_AQCOMPUTEBACKEND_H
#define AQUA_AQCOMPUTEBACKEND_H

// AQUA Phase 5 — the GPU compute backend (internal; pimpl-only, never in
// include/aqua/*). This is the one AQUA-side object that holds the OmegaGTE
// handles physics kernels are dispatched through: the OmegaGraphicsEngine
// (which makes compute pipelines + buffers) and the GECommandQueue (which
// submits the dispatches). AQ-prefixed, no namespace, per aqua/AGENTS.md.
//
// Phase 5a: own the handles, report usability. Phase 5b: kernel library load
// + capability probe (selfTest). Phase 5c (this revision): the pooled body
// buffers (struct-of-float4 component groups mirroring AQBodySoA), the
// upload/download paths, and the first real dispatch — the integration
// half-step kernels encoded per sub-step (encodeIntegrate).
//
// `usable()` stays false until the FULL GPU step is ported (5f/5g flips it) —
// otherwise `executionPath()` would claim GPU while AQSpace still steps on
// the CPU. The 5c-5f stage entry points below are exercised by the
// stage-isolation parity tests (plan §9) ahead of that flip.
//
// Capability-gate note: OmegaGraphicsEngine does NOT expose its GTEDevice or
// GTEDeviceFeatures (only `underlyingNativeDevice()` → void* and a protected
// feature bitmask), so the gate cannot read `GTEDeviceFeatures` off the engine.
// The honest probe is behavioural — `selfTest` builds and runs a trivial
// compute pipeline end to end.

#include <aqua/AQBase.h>   // AQUA_NODISCARD
#include <omega-common/utils.h>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace OmegaGTE {
    class OmegaGraphicsEngine;
    class GECommandQueue;
    class GEBuffer;
    /// GEPipeline.h spells this as a using-alias of an incomplete struct —
    /// repeat the same alias (a plain `class` forward-decl would clash).
    using GEComputePipelineState = struct __GEComputePipelineState;
    struct GTEShaderLibrary;
}

struct AQBodySoA;

/// Owns the OmegaGTE handles AQUA dispatches physics compute kernels through.
struct AQUA_EXPORT AQComputeBackend {
    /// Stand up a compute backend for `engine` + `queue`.
    /// @returns nullptr when `engine` is null — that is a CPU-only context
    /// (`AQContext::CreateCPUOnly`), where there is no device to run kernels on.
    /// Otherwise a backend that *holds* the engine and queue; whether the GPU
    /// path is actually selectable is reported by `usable()`.
    static std::unique_ptr<AQComputeBackend> TryCreate(
        SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
        SharedHandle<OmegaGTE::GECommandQueue> queue);

    ~AQComputeBackend();

    /// Whether the GPU path can run the *full step* right now. Still false
    /// through 5c-5e — the stage kernels land incrementally behind the
    /// stage-isolation parity tests; 5f/5g flips this true once the GPU step
    /// is end-to-end.
    AQUA_NODISCARD bool usable() const { return gpuUsable; }

    /// The held engine/queue, for the buffer pools and pipeline build.
    AQUA_NODISCARD SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine() const { return gpuEngine; }
    AQUA_NODISCARD SharedHandle<OmegaGTE::GECommandQueue> queue() const { return cmdQueue; }

    /// Load AQUA's precompiled kernel library (`AQKernels.omegasllib`) from
    /// @p path via `OmegaGraphicsEngine::loadShaderLibrary`. Returns false if the
    /// engine is null or the load fails. (Phase 5b precompile path, plan §7.5.)
    bool loadKernelLibrary(const OmegaCommon::String& path);

    /// Capability probe: build a compute pipeline for the `AQProbeDouble` kernel,
    /// dispatch it over a small buffer, and verify the GPU doubled the values.
    /// Proves the precompile -> load -> pipeline -> buffer -> dispatch ->
    /// readback toolchain end to end on the active device. Requires
    /// `loadKernelLibrary` to have succeeded and a non-null command queue.
    /// Returns true iff the GPU produced the expected results.
    AQUA_NODISCARD bool selfTest();

    // --- Phase 5c: body-state GPU mirror + integration dispatch ---

    /// Grow (never shrink) the pooled body buffers to hold at least
    /// @p bodyCount bodies. Geometric growth; no per-sub-step allocation once
    /// warm (roadmap §6). Returns false when the engine is null or an
    /// allocation fails.
    bool ensureBodyCapacity(std::size_t bodyCount);

    /// Host SoA -> the pooled GPU body buffers (all `soa.size()` bodies).
    /// Grows capacity as needed. The split-impulse pseudo-velocity array
    /// uploads alongside the dynamics state (zero when no solver ran).
    bool uploadBodies(const AQBodySoA& soa);

    /// GPU body buffers -> host SoA (the §6.I readback). Overwrites only the
    /// GPU-mirrored dynamics fields of @p soa (which must already be sized to
    /// the uploaded body count); host-only fields (COM offset) are untouched.
    bool downloadBodies(AQBodySoA& soa);

    /// Encode + submit @p substeps back-to-back integration sub-steps
    /// (AQIntegrateVelocity then AQIntegratePosition per sub-step, one thread
    /// per body) over the currently-uploaded body buffers, and block until
    /// the GPU finishes. First real dispatch of the ported step (plan §13 5c);
    /// the full pipeline interleaves the other stage kernels between the two
    /// half-steps in later increments.
    bool encodeIntegrate(float dt, const float gravity[3],
                         std::size_t bodyCount, unsigned substeps);

    // --- Phase 5d: broadphase (AABB refresh + sort-based grid) ---

    /// Per-body collision inputs for the broadphase kernels, dereferenced
    /// from the space's shape table at upload time (shape handle -> flat
    /// arrays). All arrays are sized to the body count; `hullVerts` is the
    /// shared vertex pool ((x, y, z) triplets, one float4 slot each).
    struct BroadphaseInputs {
        OmegaCommon::Vector<std::uint32_t> shapeType;    ///< AQShapeType as uint
        OmegaCommon::Vector<std::uint32_t> hullFirst;    ///< first vertex (hulls)
        OmegaCommon::Vector<std::uint32_t> hullCount;    ///< vertex count (hulls)
        OmegaCommon::Vector<std::uint32_t> hasShape;     ///< 0/1
        OmegaCommon::Vector<float> paramX, paramY, paramZ, paramW; ///< per-type params
        OmegaCommon::Vector<float> localPosX, localPosY, localPosZ;
        OmegaCommon::Vector<float> localQuatX, localQuatY, localQuatZ, localQuatW;
        OmegaCommon::Vector<std::uint32_t> filterLayer, filterMask;
        OmegaCommon::Vector<float> hullVertX, hullVertY, hullVertZ;
        AQUA_NODISCARD std::size_t size() const { return shapeType.size(); }
    };

    /// Upload the per-body shape/filter tables + hull pool. Body poses and
    /// velocities ride the SAME pooled body buffers `uploadBodies` fills —
    /// call that first.
    bool uploadBroadphaseInputs(const BroadphaseInputs& in);

    /// Dispatch AQRefreshAABB over @p bodyCount bodies (world + fat bounds,
    /// margin + velocity*frameDt fattening) and wait.
    bool encodeRefreshAABB(std::size_t bodyCount, float fattenMargin, float frameDt);

    /// Read back the per-body fat AABBs (xyz min / xyz max per body, w junk).
    /// Used by the host to pick the grid cell size (max fat extent) and by
    /// the 5d parity assertions.
    bool downloadFatAABBs(OmegaCommon::Vector<float>& minXYZ,
                          OmegaCommon::Vector<float>& maxXYZ,
                          std::size_t bodyCount);

    /// Dispatch the grid chain (hash -> stable sort -> pair scan -> pair
    /// sort) over the refreshed AABBs and wait. @p cellSize MUST be >= the
    /// maximum fat-AABB extent of any grid body (the 27-neighborhood
    /// completeness bound — see AQBroadphase.omegasl header). Grows the pair
    /// pool and re-runs internally if the append overflows @p pairCapacityHint.
    bool encodeBroadphase(std::size_t bodyCount, float cellSize,
                          std::size_t pairCapacityHint);

    /// Read back the sorted, exact-overlap-filtered pair list the grid chain
    /// produced ((a, b) with a < b, ordered by (a, b) — byte-identical to the
    /// CPU broadphase's list).
    bool downloadPairs(OmegaCommon::Vector<std::uint32_t>& pairsAB);

    // --- Phase 5e: narrowphase (count -> prefix scan -> build) ---

    /// Host mirror of the GPU manifold record (AQContactManifold shape; the
    /// 4-point arrays are fixed like the CPU POD).
    struct ManifoldOut {
        std::uint32_t a = 0, b = 0, pointCount = 0;
        float normal[3] = {0.f, 0.f, 0.f};
        float restitutionCombined = 0.f, frictionCombined = 0.f;
        float pointPos[4][3] = {};
        float pointDepth[4] = {};
        std::uint32_t pointKey[4] = {};
        float pointAccum[4][3] = {};
    };

    /// Host mirror of the GPU constraint row (AQConstraintRow shape).
    struct RowOut {
        std::uint32_t kind = 0, bodyA = 0, bodyB = 0, peerRow = 0;
        float contactPoint[3] = {}, rA[3] = {}, rB[3] = {}, direction[3] = {};
        float effectiveMass = 0.f, bias = 0.f, accumImpulse = 0.f, frictionCoeff = 0.f;
        float compliance = 0.f;
        bool isAngular = false;
    };

    /// Per-body material/trigger table (restitution, friction, isTrigger).
    bool uploadNarrowphaseInputs(const OmegaCommon::Vector<float>& restitution,
                                 const OmegaCommon::Vector<float>& friction,
                                 const OmegaCommon::Vector<std::uint32_t>& isTrigger);

    /// Warm-start persistence cache: entries sorted ascending by
    /// (hi = featureKey, lo = a | b<<16); values are the accumulated
    /// (normal, friction0, friction1) impulses. Pass count 0 to run cold.
    bool uploadWarmStartCache(const OmegaCommon::Vector<std::uint32_t>& keysLoHi,
                              const OmegaCommon::Vector<float>& valsNF0F1);

    /// Dispatch the narrowphase over the pair list currently in the
    /// broadphase's sorted-pair buffer: count pass, two inclusive prefix
    /// scans (contact flags + point counts, Hillis-Steele ping-pong), build
    /// pass — one command buffer, one sync. Consumes the body/shape tables
    /// already uploaded by uploadBodies/uploadBroadphaseInputs.
    bool encodeNarrowphase(std::size_t pairCount, float dt,
                           std::uint32_t restCombineMode, std::uint32_t fricCombineMode,
                           std::size_t cacheCount);

    /// Read back the dense pair-ordered manifolds + rows and the per-pair
    /// CPU-fallback flags (the GJK/EPA pairs the GPU skipped).
    bool downloadNarrowphase(OmegaCommon::Vector<ManifoldOut>& manifolds,
                             OmegaCommon::Vector<RowOut>& rows,
                             OmegaCommon::Vector<std::uint32_t>& cpuFallback,
                             std::size_t pairCount);

    // --- Phase 5f: colored constraint solver ---

    /// One constraint group = one contiguous row span sharing a body pair
    /// (a contact manifold or a joint). `manifoldIndex` indexes the 5e
    /// manifold buffer for contact groups (the position solve consumes it);
    /// pass 0xFFFFFFFF for joint groups. `finiteA/B` mark finite-mass bodies
    /// — the coloring conflict rule (the solver writes velocity into movable
    /// bodies and split-impulse pseudo state into ANY finite-mass body, so
    /// two groups sharing a finite-mass body must not share a color).
    struct SolveGroupIn {
        std::uint32_t firstRow = 0, rowCount = 0;
        std::uint32_t manifoldIndex = 0xFFFFFFFFu;
        std::uint32_t bodyA = 0, bodyB = 0;
        bool finiteA = false, finiteB = false;
    };

    /// Greedy-color the groups (deterministic: group order, first free color
    /// — plan §11.3 CPU-side first cut), upload the group table + the
    /// by-color index list, and remember the color ranges for encodeSolve.
    /// Emits the §9 degenerate-coloring loud guard when the color count
    /// approaches the group count.
    bool setSolveGroups(const OmegaCommon::Vector<SolveGroupIn>& groups);

    /// Encode + submit the full solve for one sub-step over the rows/
    /// manifolds currently in the 5e buffers: optional warm-start pass (the
    /// CPU's pass D), `velocityIters` colored PGS sweeps (pass E), then the
    /// split-impulse position solve (`positionIters` colored sweeps, pass F)
    /// into freshly-zeroed pseudoLinear/pseudoAngular body buffers. One
    /// command buffer, one sync.
    bool encodeSolve(float dt, int velocityIters, int positionIters,
                     bool warmStart, std::size_t bodyCount);

    /// The two integrator half-steps as separate dispatches, so a caller can
    /// interleave the contact solve between them exactly like the CPU
    /// sub-step (velocity -> narrowphase+solve -> position). encodeIntegrate
    /// remains the fused non-contact form.
    bool encodeVelocityHalfStep(float dt, const float gravity[3], std::size_t bodyCount);
    bool encodePositionHalfStep(float dt, const float gravity[3], std::size_t bodyCount);

private:
    AQComputeBackend(SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
                     SharedHandle<OmegaGTE::GECommandQueue> queue);

    /// Build-or-fetch the cached compute pipeline for a kernel in the loaded
    /// library. Returns null (loudly) when the library is not loaded, the
    /// kernel is missing, or the pipeline build fails.
    SharedHandle<OmegaGTE::GEComputePipelineState> pipeline(const std::string& name);

    SharedHandle<OmegaGTE::OmegaGraphicsEngine> gpuEngine;
    SharedHandle<OmegaGTE::GECommandQueue>      cmdQueue;
    SharedHandle<OmegaGTE::GTEShaderLibrary>    kernelLib;   ///< loaded AQKernels.omegasllib
    bool                                        gpuUsable = false;

    std::map<std::string, SharedHandle<OmegaGTE::GEComputePipelineState>> pipelines;

    /// Pooled per-body buffers — one float4 per body per buffer, the
    /// struct-of-float4 component groups the kernels bind at slots 1..8
    /// (see AQIntegrate.omegasl for the w-lane packing).
    ///
    /// Buffer residency follows the GTE cross-backend contract (plan §8 +
    /// gte D3D12-CPU-Accessible-Buffer-Plan): a buffer some kernel declares
    /// `out`/`inout` is GPU-written and cannot be `Upload` usage (on D3D12
    /// UPLOAD-heap memory cannot be a UAV and records no barriers, which is
    /// what raced the 5c-5f chains). GPU-written buffers the CPU only reads
    /// back are `Readback`; buffers the CPU ALSO writes (the body pool, the
    /// pair counter) are `Universal` — the backend owns the staging there
    /// (D3D12 keeps an UPLOAD companion it flushes into the primary at bind
    /// time; the docs flag Universal as the most expensive usage, which is
    /// exactly the shared-both-ways body state this pool holds). Kernel
    /// inputs the CPU owns stay `Upload`.
    SharedHandle<OmegaGTE::GEBuffer> bodyPosAct;    ///< slot 1 (Universal)
    SharedHandle<OmegaGTE::GEBuffer> bodyVelIm;     ///< slot 2 (Universal)
    SharedHandle<OmegaGTE::GEBuffer> bodyQuat;      ///< slot 3 (Universal)
    SharedHandle<OmegaGTE::GEBuffer> bodyWbMax;     ///< slot 4 (Universal)
    SharedHandle<OmegaGTE::GEBuffer> bodyInvIGs;    ///< slot 5 (input-only, Upload)
    SharedHandle<OmegaGTE::GEBuffer> bodyForceLd;   ///< slot 6 (Universal)
    SharedHandle<OmegaGTE::GEBuffer> bodyTorqueAd;  ///< slot 7 (Universal)
    SharedHandle<OmegaGTE::GEBuffer> bodyPseudo;    ///< slot 8 (Universal; GPU-written by 5f)
    SharedHandle<OmegaGTE::GEBuffer> bodyCom;       ///< COM offset (5e; narrowphase slot 6, input-only, Upload)
    std::size_t bodyCapacity = 0;

    /// 5e narrowphase pools (slots per AQNarrowphase.omegasl).
    bool ensureNarrowphaseCapacity(std::size_t pairCount);
    SharedHandle<OmegaGTE::GEBuffer> npMaterial;                    ///< float4 / body
    SharedHandle<OmegaGTE::GEBuffer> npContactFlag, npPointCount, npCpuFallback; ///< uint / pair
    SharedHandle<OmegaGTE::GEBuffer> npScanA, npScanB, npScanC, npScanD; ///< uint / pair (two ping-pong sets)
    SharedHandle<OmegaGTE::GEBuffer> npManifolds;                   ///< 192 B / pair (max)
    SharedHandle<OmegaGTE::GEBuffer> npRows;                        ///< 112 B / (12 * pair) (max)
    SharedHandle<OmegaGTE::GEBuffer> npCacheKeys, npCacheVals;      ///< warm-start cache
    SharedHandle<OmegaGTE::GEBuffer> npScanFlagFinal, npScanPtsFinal; ///< aliases of the final ping-pong buffers
    std::size_t npPairCapacity = 0;
    std::size_t npCacheCapacity = 0;

    /// 5f solver state (slots per AQSolver.omegasl).
    SharedHandle<OmegaGTE::GEBuffer> bodyPseudoAng;   ///< split-impulse angular accum (Universal — CPU-zeroed, GPU-accumulated)
    SharedHandle<OmegaGTE::GEBuffer> svGroups;        ///< uint4 / group
    SharedHandle<OmegaGTE::GEBuffer> svGroupsByColor; ///< uint / group
    std::size_t svGroupCapacity = 0;
    OmegaCommon::Vector<std::pair<std::uint32_t, std::uint32_t>> svColorRanges;

    /// 5d broadphase pools (slots per AQBroadphase.omegasl).
    bool ensureBroadphaseCapacity(std::size_t bodyCount, std::size_t hullVertCount);
    bool ensurePairCapacity(std::size_t pairCapacity);
    bool runBroadphaseChain(std::size_t bodyCount, float cellSize,
                            std::size_t pairCapacity, std::uint32_t& rawCount);
    SharedHandle<OmegaGTE::GEBuffer> bpShapeInfo;      ///< uint4 / body
    SharedHandle<OmegaGTE::GEBuffer> bpShapeParams;    ///< float4 / body
    SharedHandle<OmegaGTE::GEBuffer> bpShapeLocalPos;  ///< float4 / body
    SharedHandle<OmegaGTE::GEBuffer> bpShapeLocalQuat; ///< float4 / body
    SharedHandle<OmegaGTE::GEBuffer> bpFilter;         ///< uint4 / body
    SharedHandle<OmegaGTE::GEBuffer> bpHullVerts;      ///< float4 / hull vertex
    SharedHandle<OmegaGTE::GEBuffer> bpWorldMin, bpWorldMax, bpFatMin, bpFatMax; ///< float4 / body
    SharedHandle<OmegaGTE::GEBuffer> bpEntries, bpEntriesSorted;   ///< uint2 / body
    SharedHandle<OmegaGTE::GEBuffer> bpCellCoords;     ///< int4 / body
    SharedHandle<OmegaGTE::GEBuffer> bpEntryRanks;     ///< uint / body
    SharedHandle<OmegaGTE::GEBuffer> bpPairCount;      ///< single uint (atomic; Universal — CPU-zeroed, GPU-appended)
    SharedHandle<OmegaGTE::GEBuffer> bpPairs, bpPairsSorted; ///< uint2 / pair
    SharedHandle<OmegaGTE::GEBuffer> bpPairRanks;      ///< uint / pair
    std::size_t bpBodyCapacity = 0;
    std::size_t bpHullVertCapacity = 0;
    std::size_t bpPairCapacity = 0;
};

#endif // AQUA_AQCOMPUTEBACKEND_H
