#ifndef OMEGAWTK_UI_FRAMEPACER_H
#define OMEGAWTK_UI_FRAMEPACER_H

#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Native/NativeScreen.h"   // NativeDisplayLinkPtr (§2.9)
#include "AnimationScheduler.h"             // FrameTime

#include <atomic>
#include <cstdint>

namespace OmegaWTK {

class AppWindow;

// UIView-Render-Redesign Phase H (folds the stale Frame-Pacing-Plan).
//
// Per-window frame pacer: the single owner of the FrameTime clock + the
// per-window frame index, and (H.2) the vsync gate that aligns frame
// production to the screen's `Native::NativeDisplayLink` (§2.9). Owned by
// `AppWindow::Impl`, a peer of `FrameBuilder` / `AnimationScheduler`.
//
// Platform-agnostic: every OS-specific concern lives behind
// `NativeDisplayLink`. The vsync signal is a property of the *screen*, so
// the pacer binds to `displayLinkForScreen(currentScreen())` and rebinds
// on cross-screen transitions (driven by Phase F's `onRealize`).
//
// Threading discipline (H.2): subscribe / unsubscribe happen ONLY on the
// UI thread (`requestBuild` and `onFrameFlushed`). The display-link
// callback (`onVsync`) never mutates the subscription — it only touches
// atomics and `requestFrameFlush` (itself a thread-safe UI-thread-
// marshalling primitive) — so there is no re-entrant "unsubscribe from
// inside my own callback" hazard, and §2.9 needs no change. The callback
// fires on the GTK main/UI thread on Linux but on a dedicated link thread
// on macOS/Win, which is why `buildPending_` and the vsync sample are
// `std::atomic`.
class FramePacer {
public:
    explicit FramePacer(AppWindow & window);
    ~FramePacer();

    FramePacer(const FramePacer &)             = delete;
    FramePacer & operator=(const FramePacer &) = delete;

    // Bind to the display link for the screen the owning AppWindow
    // currently lives on. Called once at construction and again on every
    // cross-screen transition. Rebinding while a subscription is live
    // moves the subscription to the new link (the FrameTime clock is OS-
    // monotonic, not the link's, so it survives the rebind without a jump
    // — only the interval changes).
    void bindTo(Native::NativeDisplayLinkPtr link);

    // Stamp the FrameTime for the frame about to be built and advance the
    // per-window frame index. Called once per outermost frame from
    // `FrameBuilder::beginFrame` (Tick phase). H.1: { steadyNowNs(),
    // ++frameIndex }. H.2 (vsync-active): the predicted presentation time
    // of the frame we are about to build.
    FrameTime beginFrameTime();

    // The last-stamped FrameTime (read-back accessor).
    FrameTime currentFrameTime() const { return lastFrameTime_; }

    // Expected frame interval, forwarded from the bound display link
    // (16'666'666 @ 60 Hz). Nominal 60 Hz fallback when unbound.
    std::uint64_t expectedFrameIntervalNs() const;

    // True iff vsync-aligned production is enabled
    // (OMEGAWTK_VSYNC_PACING, read once at construction).
    bool vsyncPacingEnabled() const { return vsyncPacing_; }

    // ---- H.2: vsync-aligned production (UI thread) ------------------

    // Arm "build on the next vsync" and ensure the link is subscribed.
    // Called from `AppWindow::requestFrame` when vsync pacing is on.
    void requestBuild();

    // Called at the tail of `AppWindow::flushFrame`. Pauses (unsubscribes)
    // the link iff no build is pending and no animation is active, so a
    // resting app costs zero per-vsync wakeups. A fresh invalidate
    // re-subscribes via `requestBuild`.
    void onFrameFlushed();

private:
    void ensureSubscribed();   // UI thread
    void pause();              // UI thread
    void onVsync(std::uint64_t presentationTimeNs,
                 std::uint64_t intervalNs);   // display-link thread

    std::uint64_t steadyNowNs() const;
    bool animationsActive() const;

    AppWindow & window_;
    Native::NativeDisplayLinkPtr link_;       // UI thread (bindTo)

    bool          vsyncPacing_ = false;       // env gate, immutable post-ctor
    bool          subscribed_  = false;       // UI thread only

    std::uint32_t frameIndex_  = 0;           // UI thread (beginFrameTime)
    FrameTime     lastFrameTime_{};           // UI thread

    std::atomic<bool>          buildPending_{false};
    std::atomic<std::uint64_t> lastPresentationNs_{0};
    std::atomic<std::uint64_t> lastIntervalNs_{0};
    std::atomic<bool>          haveVsyncSample_{false};
};

}

#endif
