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

};
