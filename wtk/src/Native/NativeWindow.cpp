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

};
