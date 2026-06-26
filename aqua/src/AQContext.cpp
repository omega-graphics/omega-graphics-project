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

AQContext::~AQContext() = default;  // defined here: AQComputeBackend is complete

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

void AQContext::setExecutionPath(AQExecPath path) {
    requestedPath = path;
    if (path == AQExecPath::GPU && !(compute && compute->usable())) {
        // Asked for GPU but no usable backend (no engine, or kernels not yet
        // available in this build). Loud once-per-call so the fallback is never
        // silent; the resolved path stays CPU.
        std::cerr << "AQUA::AQContext: GPU execution requested but no usable "
                     "compute backend — running the CPU path.\n";
    }
}

AQExecPath AQContext::executionPath() const {
    if (requestedPath == AQExecPath::CPU) {
        return AQExecPath::CPU;
    }
    const bool gpuUsable = compute && compute->usable();
    // Auto and GPU both resolve to GPU only when a usable backend exists; the
    // GPU-request warning is emitted in setExecutionPath, so this stays const.
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

    while (accumulator >= fixedDt) {
        for (auto &space : spaces) space->stepInternal(fixedDt);
        accumulator -= fixedDt;
        elapsedTime += fixedDt;
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
