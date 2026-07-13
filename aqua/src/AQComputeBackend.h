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

#include <aqua/AQBase.h>   // OMEGA_NODISCARD
#include <omega-common/utils.h>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace OmegaGTE {
    class OmegaGraphicsEngine;
    class GECommandQueue;
    class GECommandBuffer;
    class GEBuffer;
    /// GEPipeline.h spells this as a using-alias of an incomplete struct —
    /// repeat the same alias (a plain `class` forward-decl would clash).
    using GEComputePipelineState = struct __GEComputePipelineState;
    struct GTEShaderLibrary;
}

struct AQBodySoA;
struct AQForceField;

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
    OMEGA_NODISCARD bool usable() const { return gpuUsable; }

    /// The held engine/queue, for the buffer pools and pipeline build.
    OMEGA_NODISCARD SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine() const { return gpuEngine; }
    OMEGA_NODISCARD SharedHandle<OmegaGTE::GECommandQueue> queue() const { return cmdQueue; }

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
    OMEGA_NODISCARD bool selfTest();

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
        OMEGA_NODISCARD std::size_t size() const { return shapeType.size(); }
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

    // --- Phase 7f: XPBD constraint projection (AQComputeXPBD.cpp) ---
    // One resident pool per XPBD body (keyed by the body's opaque id — the
    // particle-pool idiom): particle SoA + color-sorted constraints stay
    // device-resident across slices, sub-steps, AND frames; uploads happen on
    // topology change only, downloads at the readback sync points.

    /// One color-sorted distance constraint for upload (mirrors
    /// AQDistanceConstraint minus λ, which starts 0 on-device).
    struct XPBDConstraintIn {
        std::uint32_t a = 0, b = 0;
        float restLength = 0.f, compliance = 0.f;
        std::uint32_t color = 0;
    };

    /// Upload body @p id's particle SoA (positions+invMass, velocities;
    /// x_prev is device-initialized to x). Creates/grows the body's pool;
    /// positions ride the pos buffer's xyz with invMass in w (pinned = 0).
    bool uploadXPBDParticles(std::uint64_t id,
                             const OmegaCommon::Vector<float>& posX,
                             const OmegaCommon::Vector<float>& posY,
                             const OmegaCommon::Vector<float>& posZ,
                             const OmegaCommon::Vector<float>& invMass,
                             const OmegaCommon::Vector<float>& velX,
                             const OmegaCommon::Vector<float>& velY,
                             const OmegaCommon::Vector<float>& velZ);

    /// Upload body @p id's COLOR-SORTED constraint array + its color ranges
    /// (contiguous per color — AQXPBDBody's `distanceSorted`/`batches`
    /// layout). Uploaded on topology change only; per-constraint guard-trip
    /// counters reset to 0.
    bool uploadXPBDConstraints(std::uint64_t id,
                               const OmegaCommon::Vector<XPBDConstraintIn>& sorted,
                               const OmegaCommon::Vector<std::pair<std::uint32_t, std::uint32_t>>& colorRanges);

    /// Encode + submit @p engineSubsteps back-to-back engine sub-steps of
    /// XPBD for body @p id: each sub-step is `substeps` slices of
    /// h = dt/substeps, each slice predict → λ-reset → iterations ×
    /// (one dispatch per color, colors serial) → derive — all in ONE command
    /// buffer, one sync (the live path encodes a whole advance-frame at once).
    bool encodeXPBDAdvance(std::uint64_t id, float dt, const float gravity[3],
                           std::uint32_t substeps, std::uint32_t iterations,
                           float velocityDamping, float explosionThreshold,
                           std::size_t particleCount,
                           std::uint32_t engineSubsteps = 1);

    /// Read back body @p id's particle positions + velocities (the
    /// readXPBDState / debug-draw sync point, and the parity assertions).
    bool downloadXPBDParticles(std::uint64_t id,
                               OmegaCommon::Vector<float>& posX,
                               OmegaCommon::Vector<float>& posY,
                               OmegaCommon::Vector<float>& posZ,
                               OmegaCommon::Vector<float>& velX,
                               OmegaCommon::Vector<float>& velY,
                               OmegaCommon::Vector<float>& velZ,
                               std::size_t particleCount);

    /// Read back body @p id's cumulative per-constraint explosion-guard trip
    /// counters (the host reports loudly + feeds AQDebugConstraint, like the
    /// CPU path).
    bool downloadXPBDTrips(std::uint64_t id, OmegaCommon::Vector<std::uint32_t>& trips);

    /// Drop a destroyed body's pool (no-op on an unknown id).
    void xpbdReleasePool(std::uint64_t id);

    // --- Phase 6g: reusable exclusive prefix scan (AQComputeParticles.cpp) ---

    /// Standalone exclusive prefix sum over uints: upload @p in, run the
    /// multi-pass reduce-then-scan chain (AQScan.omegasl), download into
    /// @p out. Integer adds ⇒ the result is bit-exact, run-to-run and
    /// cross-device. This is the stage-test surface; the particle compaction
    /// (6h) reuses the same chain via encodeScanExclusive on its own buffers.
    bool scanExclusive(const OmegaCommon::Vector<std::uint32_t>& in,
                       OmegaCommon::Vector<std::uint32_t>& out);

    // --- Phase 6h/6i: live GPU particle path (AQComputeParticles.cpp) ---
    // One resident pool per particle system (keyed by the system's opaque
    // id), always compacted to the live prefix at frame end. The host owns
    // the integer bookkeeping (emission, death schedule, census); the pool
    // holds the float state, resident across sub-steps AND frames — the host
    // syncs only at emission (staging upload) and readback (§14.1).

    /// Whether the live GPU pillars (Phase 6h particles + Phase 7f XPBD) are
    /// selectable: kernel library loaded + capability probe passed
    /// (AQContext::loadKernels flips it). Distinct from usable(), which
    /// remains the full-rigid-step gate.
    OMEGA_NODISCARD bool kernelsLive() const { return kernelsLiveFlag; }
    void setKernelsLive(bool on) { kernelsLiveFlag = on; }

    /// One static collider for the 6i collide kernel — the shape's COMBINED
    /// world transform (body ∘ shape-local), its type + params (plane normal
    /// pre-normalized host-side with the CPU's float math), and restitution.
    /// Hulls are filtered out by the caller (their SDF is +inf on the CPU
    /// path too).
    struct ParticleColliderIn {
        std::uint32_t shapeType = 0;     ///< AQShapeType as uint (never ConvexHull)
        float px = 0.f, py = 0.f, pz = 0.f;
        float qx = 0.f, qy = 0.f, qz = 0.f, qw = 1.f;
        float params[4] = {0.f, 0.f, 0.f, 0.f};
        float restitution = 0.f;
    };

    /// One advance-frame's encode parameters (§14.3 6h). The whole frame —
    /// inject → substeps × (integrate [+ collide] + age) → scan → scatter →
    /// flag rebuild — rides one command buffer with one sync.
    struct ParticleFrameDesc {
        std::uint64_t systemId = 0;
        float         dt = 0.f;          ///< engine fixed sub-step
        std::uint32_t substeps = 0;      ///< sub-steps this advance runs
        std::uint32_t activeSpan = 0;    ///< occupied prefix: live-at-frame-start + injected
        std::uint32_t injectStart = 0;   ///< first tail slot of this frame's new particles
        std::uint32_t injectCount = 0;
        std::uint32_t newLiveCount = 0;  ///< host-predicted census after this frame's deaths
        std::uint32_t fieldCount = 0;
        std::uint32_t colliderCount = 0; ///< 0 ⇒ collision pass skipped
    };

    /// Create (or re-create at a larger capacity) the resident pool for a
    /// system; freshly-created flags are device-zeroed. Returns false when
    /// the engine is null or allocation fails.
    bool particlesEnsurePool(std::uint64_t id, std::uint32_t capacity);
    /// Drop a destroyed system's pool (no-op on an unknown id).
    void particlesReleasePool(std::uint64_t id);

    /// Upload this frame's host-sampled new particles into the emission
    /// staging buffers (lane arrays; `radius` rides posR.w, `invMass`
    /// velIM.w, `death` is the 6f integer countdown).
    bool particlesUploadStaging(std::uint64_t id,
                                const float* posX, const float* posY, const float* posZ,
                                const float* radius,
                                const float* velX, const float* velY, const float* velZ,
                                const float* invMass,
                                const std::uint32_t* death, std::size_t count);

    /// Upload the system's force fields (all of them, enabled flag included —
    /// the kernel replicates the CPU's per-field enabled check).
    bool particlesUploadFields(std::uint64_t id,
                               const OmegaCommon::Vector<AQForceField>& fields);

    /// Upload the per-advance static-collider snapshot (6i).
    bool particlesUploadColliders(std::uint64_t id,
                                  const OmegaCommon::Vector<ParticleColliderIn>& colliders);

    /// Encode + submit one advance-frame of device work and wait; swaps the
    /// pool's ping-pong buffers so the packed live prefix is current.
    bool particlesEncodeFrame(const ParticleFrameDesc& desc);

    /// Read back the packed live prefix's positions + velocities (the
    /// readParticleState / debug-draw sync point).
    bool particlesDownloadState(std::uint64_t id,
                                float* posX, float* posY, float* posZ,
                                float* velX, float* velY, float* velZ,
                                std::size_t count);

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

    /// 6g scan state (slots per AQScan.omegasl; AQComputeParticles.cpp).
    /// The level buffers hold each recursion depth's block sums and their
    /// scans (depth ≤ 3 up to 2M elements at block size 128).
    bool ensureScanCapacity(std::size_t n);
    /// Encode the recursive reduce-then-scan chain (block scan → recurse on
    /// the sums → uniform add) into an already-open command buffer. `input`
    /// may be any uint buffer (the 6h compaction passes its flags buffer);
    /// the exclusive result lands in `output`.
    bool encodeScanExclusive(const SharedHandle<OmegaGTE::GECommandBuffer>& cmdBuf,
                             SharedHandle<OmegaGTE::GEBuffer> input,
                             SharedHandle<OmegaGTE::GEBuffer> output,
                             std::size_t n, unsigned level);
    SharedHandle<OmegaGTE::GEBuffer> scanInput, scanOutput;   ///< standalone-API staging
    OmegaCommon::Vector<SharedHandle<OmegaGTE::GEBuffer>> scanLevelSums;
    OmegaCommon::Vector<SharedHandle<OmegaGTE::GEBuffer>> scanLevelScan;
    std::size_t scanCapacity = 0;

    /// 6h/6i resident particle pools (slots per AQParticles.omegasl;
    /// AQComputeParticles.cpp). posR/velIM/death are ping-pong pairs (`cur`
    /// is the live index) so the stable-compaction scatter never aliases its
    /// source; flags/offsets are rebuilt in place each frame.
    struct ParticlePoolGPU {
        SharedHandle<OmegaGTE::GEBuffer> posR[2], velIM[2];
        SharedHandle<OmegaGTE::GEBuffer> death[2];
        SharedHandle<OmegaGTE::GEBuffer> flags, offsets;
        SharedHandle<OmegaGTE::GEBuffer> stagePosR, stageVelIM, stageDeath;
        SharedHandle<OmegaGTE::GEBuffer> fields, colliders;
        std::size_t capacity = 0;
        std::size_t stageCapacity = 0;
        std::size_t fieldCapacity = 0;
        std::size_t colliderCapacity = 0;
        int cur = 0;
    };
    ParticlePoolGPU* ptPoolAt(std::uint64_t id);
    std::map<std::uint64_t, ParticlePoolGPU> ptPools;
    bool kernelsLiveFlag = false;

    /// 7f XPBD pools, one per body id (slots per AQXPBD.omegasl;
    /// AQComputeXPBD.cpp). pos/vel are Universal (CPU-seeded, GPU-mutated,
    /// CPU-read-back); prev is GPU-only (Readback); con carries λ in the
    /// record (Universal); trips are CPU-zeroed + GPU-counted (Universal).
    struct XPBDPoolGPU {
        SharedHandle<OmegaGTE::GEBuffer> pos, prev, vel, con, trips;
        std::size_t particleCapacity = 0;
        std::size_t constraintCapacity = 0;
        std::size_t constraintCount = 0;
        OmegaCommon::Vector<std::pair<std::uint32_t, std::uint32_t>> colorRanges;
    };
    XPBDPoolGPU* xpbdPoolAt(std::uint64_t id);
    bool ensureXPBDParticleCapacity(XPBDPoolGPU& pool, std::size_t particleCount);
    bool ensureXPBDConstraintCapacity(XPBDPoolGPU& pool, std::size_t constraintCount);
    std::map<std::uint64_t, XPBDPoolGPU> xpbdPools;
};

#endif // AQUA_AQCOMPUTEBACKEND_H
