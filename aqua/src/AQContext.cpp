#include <aqua/AQContext.h>

AQContext::AQContext(SharedHandle<OmegaGTE::GECommandQueue> commandQueue)
    : commandQueue(std::move(commandQueue)),
      fixedDt(1.f / 120.f),
      accumulator(0.f),
      elapsedTime(0.0) {}

SharedHandle<AQContext> AQContext::Create(SharedHandle<OmegaGTE::GECommandQueue> commandQueue) {
    return SharedHandle<AQContext>(new AQContext(std::move(commandQueue)));
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
    while (accumulator >= fixedDt) {
        for (auto &space : spaces) space->stepInternal(fixedDt);
        accumulator -= fixedDt;
        elapsedTime += fixedDt;
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
