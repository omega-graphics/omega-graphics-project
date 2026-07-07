#include <aqua/AQContext.h>
#include "AQComputeBackend.h"

#include <iostream>

// The compute backend is the single owner of the engine + queue; it returns
// null for a null engine (a CPU-only context), so `compute` is null exactly when
// AQUA runs the CPU path.
AQContext::AQContext(SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
                     SharedHandle<OmegaGTE::GECommandQueue> commandQueue)
    : compute(AQComputeBackend::TryCreate(std::move(engine), std::move(commandQueue))),
      fixedDt(1.f / 120.f),
      accumulator(0.f),
      elapsedTime(0.0) {}

AQContext::~AQContext() {
    // The spaces hold a NON-owning pointer to this context's compute backend
    // while the GPU particle path steps them (Phase 6h). A space handle can
    // outlive its context, so sever the pointer before the backend dies.
    for (auto &space : spaces) space->detachCompute();
}

SharedHandle<AQContext> AQContext::Create(SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
                                          SharedHandle<OmegaGTE::GECommandQueue> commandQueue) {
    if (!engine) {
        // Mandatory-engine contract violation — always loud (caller-contract
        // tier). We still build a (CPU-only) context so a misuse degrades
        // rather than crashes, but the caller should use CreateCPUOnly instead.
        std::cerr << "AQUA::AQContext::Create: engine is required (kREATE is "
                     "GPU-first). For an engine-less CPU-only context use "
                     "AQContext::CreateCPUOnly(). Degrading to the CPU path.\n";
    }
    return SharedHandle<AQContext>(new AQContext(std::move(engine), std::move(commandQueue)));
}

SharedHandle<AQContext> AQContext::CreateCPUOnly() {
    return SharedHandle<AQContext>(new AQContext(
        SharedHandle<OmegaGTE::OmegaGraphicsEngine>(),
        SharedHandle<OmegaGTE::GECommandQueue>()));
}

bool AQContext::loadKernels(const OmegaCommon::String &kernelLibPath) {
    if (!compute) {
        std::cerr << "AQUA::AQContext::loadKernels: CPU-only context (no engine) — "
                     "there is no device to load kernels for.\n";
        return false;
    }
    if (!compute->loadKernelLibrary(kernelLibPath)) {
        std::cerr << "AQUA::AQContext::loadKernels: failed to load `" << kernelLibPath
                  << "` — the CPU path keeps running.\n";
        return false;
    }
    if (!compute->selfTest()) {
        std::cerr << "AQUA::AQContext::loadKernels: device capability probe failed — "
                     "the CPU path keeps running.\n";
        return false;
    }
    // Phase 6h/7f: the particle and XPBD pillars are the live GPU paths this
    // flips on. The rigid pillar's usable() stays false until its own flip.
    compute->setKernelsLive(true);
    return true;
}

void AQContext::setExecutionPath(AQExecPath path) {
    requestedPath = path;
    const bool anyGpu = compute && (compute->usable() || compute->kernelsLive());
    if (path == AQExecPath::GPU && !anyGpu) {
        // Asked for GPU but no usable backend (no engine, or kernels not yet
        // loaded — see loadKernels). Loud once-per-call so the fallback is
        // never silent; the resolved path stays CPU.
        std::cerr << "AQUA::AQContext: GPU execution requested but no usable "
                     "compute backend — running the CPU path.\n";
    }
}

AQExecPath AQContext::executionPath() const {
    if (requestedPath == AQExecPath::CPU) {
        return AQExecPath::CPU;
    }
    // Auto and GPU resolve to GPU when any live GPU pillar exists: the full
    // rigid step (usable(), still pending its flip) or the Phase 6h/7f
    // particle + XPBD pillars (kernelsLive(), flipped by loadKernels). The
    // GPU-request warning is emitted in setExecutionPath, so this stays const.
    const bool gpuUsable = compute && (compute->usable() || compute->kernelsLive());
    return gpuUsable ? AQExecPath::GPU : AQExecPath::CPU;
}

SharedHandle<AQSpace> AQContext::createSpace() {
    // AQContext is a friend of AQSpace, so it can reach the private constructor
    // directly — space creation lives here, not on AQSpace.
    auto space = SharedHandle<AQSpace>(new AQSpace());
    spaces.push_back(space);
    return space;
}

void AQContext::advance(float realDt) {
    if (realDt <= 0.f) return;

    // Cap the real time ingested in one call. Without this, a long stall (a
    // breakpoint, a GC pause, a slow frame) leaves the accumulator demanding
    // more sub-steps than there is wall-clock time to run them — each catch-up
    // frame then takes longer than the time it simulates and the simulation
    // never recovers (the "spiral of death"). Dropping simulated time is the
    // safe failure: the sim slows down rather than locking up.
    constexpr float kMaxFrameTime = 0.25f;
    if (realDt > kMaxFrameTime) realDt = kMaxFrameTime;

    accumulator += realDt;

    // Phase 3 broadphase cadence — twice per `advance` tick:
    //   * BEFORE the sub-step loop, so each sub-step's narrowphase + contact
    //     solver (`AQSpace::stepInternal` → internal `runNarrowphaseAndSolve`)
    //     has a pair list to consume. The fat-AABB margin includes
    //     `v · realDt` dilation, conservatively covering the whole-frame
    //     motion so the pair list is still a superset of any pair that
    //     forms during the sub-steps.
    //   * AFTER the sub-step loop, so the public `candidatePairs()` query
    //     reflects post-step body state — what users (and the broadphase-
    //     oracle test) expect to observe after `advance` returns.
    // Both calls are guarded by `accumulator >= fixedDt`: a frame that
    // doesn't carry enough banked time to advance any sub-step skips the
    // broadphase work entirely (no observable behaviour change).
    const bool willStep = (accumulator >= fixedDt);
    if (willStep) {
        for (auto &space : spaces) space->runBroadphase(realDt);
    }

    // Phase 6 — particle emission runs ONCE per advance, before the sub-step
    // loop, for exactly the slice that will be simulated this call (nSub sub-
    // steps × fixedDt). Counting nSub with the same float subtraction the loop
    // uses guarantees it equals the loop's iteration count, so the emitted
    // census is a deterministic function of (seed, sub-step count) — the
    // precondition for the double oracle to match (§8/§9).
    int nSub = 0;
    for (float acc = accumulator; acc >= fixedDt; acc -= fixedDt) ++nSub;
    const float simDt = static_cast<float>(nSub) * fixedDt;
    if (nSub > 0) {
        for (auto &space : spaces) space->particlesEmit(simDt, fixedDt);
    }

    // Phase 6h/7f — resolve the live-GPU executor once per advance: the GPU
    // paths run when the caller selected GPU (or Auto) AND loadKernels flipped
    // the pillars live. The timestep authority does not change — the GPU only
    // swaps WHERE the per-particle/per-constraint float arithmetic executes
    // (§14.1); neither pillar feeds the rigid step mid-frame (coupling is 7g),
    // so their device work batches at the frame boundary below.
    const bool gpuPillars = compute && compute->kernelsLive() &&
                            executionPath() == AQExecPath::GPU;

    while (accumulator >= fixedDt) {
        for (auto &space : spaces) space->stepInternal(fixedDt);
        if (gpuPillars) {
            // Host keeps only the deterministic integer bookkeeping per
            // sub-step; the float physics is encoded once per advance below.
            for (auto &space : spaces) space->particlesAgeSubstep(fixedDt);
        } else {
            for (auto &space : spaces) space->particlesSubstep(fixedDt);
        }
        // Phase 7 — XPBD constraint projection on the same fixed clock: each
        // sub-step is subdivided into params.substeps XPBD slices internally
        // (Macklin 2019 small steps), so there is still exactly ONE timestep
        // authority — this loop (the Phase 6 §14.1 rule). On the GPU path the
        // whole frame's slices encode at the frame boundary instead.
        if (!gpuPillars) {
            for (auto &space : spaces) space->xpbdSubstep(fixedDt);
        }
        accumulator -= fixedDt;
        elapsedTime += fixedDt;
    }

    // Phase 6 — stable stream compaction once per advance, after the sub-steps,
    // so readParticleState sees the live set packed into the prefix. On the
    // GPU path the whole frame's device work (inject → sub-steps → on-device
    // compaction) encodes first, then the host compaction mirrors the same
    // permutation from the same integer death schedule.
    // Phase 7 — XPBD debug-bus emission + guard-trip frame boundary, same
    // once-per-advance cadence.
    if (nSub > 0) {
        if (gpuPillars) {
            for (auto &space : spaces)
                space->particlesGpuFrame(compute.get(), fixedDt, nSub);
            for (auto &space : spaces)
                space->xpbdGpuFrame(compute.get(), fixedDt, nSub);
        }
        for (auto &space : spaces) space->particlesCompact();
        for (auto &space : spaces) space->xpbdFrameEnd();
    }

    if (willStep) {
        for (auto &space : spaces) space->runBroadphase(realDt);
        // Phase 4 — trigger Enter/Stay/Exit are diffed once per advance, on the
        // post-step candidate pairs + world AABBs the refresh above produced.
        for (auto &space : spaces) space->updateTriggers();
    }
}

float AQContext::fixedTimestep() const { return fixedDt; }

void AQContext::setFixedTimestep(float dt) {
    if (dt > 0.f && dt != fixedDt) {
        fixedDt = dt;
        // The sub-step drives ‖ω‖·dt, so a changed step can flip a body into (or
        // out of) the inaccurate regime — re-arm each space's per-body fast-spin
        // warning so genuine new violations are reported.
        for (auto &space : spaces) space->resetStepWarnings();
    }
}

double AQContext::elapsed() const { return elapsedTime; }
