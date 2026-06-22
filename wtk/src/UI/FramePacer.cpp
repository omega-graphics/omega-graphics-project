#include "FramePacer.h"
#include "AppWindowImpl.h"          // AppWindow::Impl (nativeWindow, animationScheduler_)
#include "omegaWTK/UI/AppWindow.h"

#include <chrono>
#include <cstdlib>                  // std::getenv

namespace OmegaWTK {

namespace {

// 60 Hz fallback when no display link is bound (or the link reports 0).
constexpr std::uint64_t kNominalIntervalNs = 16'666'666ull;

// Phase H.2 env gate. Default on.
bool readVsyncPacingEnv(){
    return true;
    // auto v = OmegaCommon::getEnvVar("OMEGAWTK_VSYNC_PACING");
    // if(!v.has_value()){
    //     return false;
    // }
    // return v != "0";
}

}

FramePacer::FramePacer(AppWindow & window): window_(window) {
    vsyncPacing_ = readVsyncPacingEnv();
}

FramePacer::~FramePacer(){
    // Detach the link subscription before we die so the captured `this`
    // can never be invoked after destruction.
    pause();
}

void FramePacer::bindTo(Native::NativeDisplayLinkPtr link){
    // Cross-screen rebind (Phase F's onRealize) or first bind (ctor).
    // Drop the old link's subscription first, then re-subscribe the new
    // link iff we were actively driving frames.
    const bool wasSubscribed = subscribed_;
    pause();                       // unsubscribe the old link (no-op if none)
    link_ = std::move(link);
    if(wasSubscribed){
        ensureSubscribed();        // move the subscription to the new link
    }
}

std::uint64_t FramePacer::steadyNowNs() const {
    // std::chrono::steady_clock is the portable C++17 monotonic clock —
    // it wraps mach_absolute_time / QueryPerformanceCounter /
    // clock_gettime(CLOCK_MONOTONIC) per platform, so the FrameTime clock
    // is uniform across backends and survives a cross-screen rebind.
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t FramePacer::expectedFrameIntervalNs() const {
    if(link_ != nullptr){
        const std::uint64_t iv = link_->expectedFrameIntervalNs();
        if(iv != 0){
            return iv;
        }
    }
    return kNominalIntervalNs;
}

FrameTime FramePacer::beginFrameTime(){
    std::uint64_t monotonicNs;
    if(vsyncPacing_ && haveVsyncSample_.load(std::memory_order_acquire)){
        // Predicted presentation time of the frame we are about to build:
        // the last vsync's timestamp plus one interval (≈ the next vsync,
        // when this paint will actually be shown). OS-monotonic based, so
        // animations step against display time, not CPU time.
        monotonicNs = lastPresentationNs_.load(std::memory_order_relaxed)
                    + lastIntervalNs_.load(std::memory_order_relaxed);
    } else {
        monotonicNs = steadyNowNs();
    }
    lastFrameTime_ = FrameTime{monotonicNs, frameIndex_++};
    return lastFrameTime_;
}

bool FramePacer::animationsActive() const {
    auto * impl = window_.impl_.get();
    if(impl == nullptr || impl->animationScheduler_ == nullptr){
        return false;
    }
    const auto s = impl->animationScheduler_->stats();
    return (s.activeProperty + s.activeCallback) > 0;
}

void FramePacer::requestBuild(){
    buildPending_.store(true, std::memory_order_release);
    ensureSubscribed();
}

void FramePacer::ensureSubscribed(){
    if(subscribed_ || link_ == nullptr){
        return;
    }
    link_->subscribe([this](std::uint64_t presentationTimeNs,
                            std::uint64_t intervalNs){
        onVsync(presentationTimeNs, intervalNs);
    });
    subscribed_ = true;
}

void FramePacer::pause(){
    if(subscribed_ && link_ != nullptr){
        link_->unsubscribe();
    }
    subscribed_ = false;
}

void FramePacer::onVsync(std::uint64_t presentationTimeNs,
                         std::uint64_t intervalNs){
    // Runs on the display-link thread (GTK: the UI/main thread; macOS/Win:
    // a dedicated link thread). Touches only atomics + requestFrameFlush
    // (a thread-safe UI-thread-marshalling primitive). It deliberately
    // does NOT read `link_` or mutate the subscription, so there is no
    // data race on `link_` and no re-entrant unsubscribe.
    if(intervalNs == 0){
        intervalNs = kNominalIntervalNs;
    }
    lastPresentationNs_.store(presentationTimeNs, std::memory_order_relaxed);
    lastIntervalNs_.store(intervalNs, std::memory_order_relaxed);
    haveVsyncSample_.store(true, std::memory_order_release);

    // Cap production at one build per vsync: consume the pending flag and
    // marshal a single coalesced flush to the UI thread. Bursts of
    // invalidate between vsyncs collapse into this one build.
    bool expected = true;
    if(buildPending_.compare_exchange_strong(expected, false,
                                             std::memory_order_acq_rel)){
        auto * impl = window_.impl_.get();
        if(impl != nullptr && impl->nativeWindow != nullptr){
            impl->nativeWindow->requestFrameFlush();
        }
    }
}

void FramePacer::onFrameFlushed(){
    // UI thread, tail of flushFrame (after the ScopedFrame's endFrame has
    // run, so the end-of-frame animation auto-pump has already had its
    // chance to re-arm via requestBuild). If nothing is pending and no
    // animation is live, pause the link so a resting app costs zero
    // per-vsync wakeups; a future invalidate re-subscribes.
    if(buildPending_.load(std::memory_order_acquire)){
        return;
    }
    if(animationsActive()){
        return;
    }
    pause();
}

}
