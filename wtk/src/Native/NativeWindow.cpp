#include "omegaWTK/Native/NativeWindow.h"


namespace OmegaWTK::Native {

    NativeWindow::NativeWindow(Composition::Rect rect, NativeEventEmitter *emitter)
        : rect(rect), windowEventEmitter(emitter) {}

    bool NativeWindow::hasEventEmitter() const {
        return windowEventEmitter != nullptr;
    }

    NativeEventEmitter *NativeWindow::eventEmitter() const {
        return windowEventEmitter;
    }

    void NativeWindow::setFrameFlushCallback(std::function<void()> cb) {
        frameFlushCallback_ = std::move(cb);
    }

    void NativeWindow::requestFrameFlush() {
        // Base: no run loop to coalesce onto, so invoke synchronously.
        // Platforms with a run loop (macOS) override to defer + coalesce.
        if(frameFlushCallback_) {
            frameFlushCallback_();
        }
    }

    float NativeWindow::scaleFactor() const {
        // §2.9 NativeScreen: canonical scale source is the screen the
        // window currently lives on. Backends no longer override this;
        // they implement the emit machinery for WindowScaleFactorChanged
        // (notify::scale-factor / WM_DPICHANGED /
        // -windowDidChangeBackingProperties:) but the getter goes
        // through one path.
        return currentScreen().scaleFactor;
    }

    NativeScreenDesc NativeWindow::currentScreen() const {
        // Resolve "which screen is this window on" by intersecting the
        // window rect's center with the enumerated screen frames. The
        // center is robust against off-screen edges or partially
        // visible windows in a way the origin alone is not. Backends
        // may override this with a faster native API
        // (`[NSWindow screen]`, `MonitorFromWindow`,
        // `gdk_display_get_monitor_at_window`) but the default impl is
        // correct on all three platforms today.
        Composition::Rect r = getRect();
        const float cx = r.pos.x + (r.w * 0.5f);
        const float cy = r.pos.y + (r.h * 0.5f);
        for(const auto & s : enumerateScreens()){
            if(cx >= s.frame.pos.x && cx < s.frame.pos.x + s.frame.w &&
               cy >= s.frame.pos.y && cy < s.frame.pos.y + s.frame.h){
                return s;
            }
        }
        // Off-screen or no screens enumerated — primary is always a
        // valid target per `NativeScreen.h`'s contract.
        return primaryScreen();
    }

};
