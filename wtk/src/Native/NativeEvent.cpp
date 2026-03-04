#include "omegaWTK/Native/NativeEvent.h"
#include <algorithm>

namespace OmegaWTK::Native {

WindowWillResize::WindowWillResize(Core::Rect r, std::uint64_t gen) : rect(r), generation(gen) {}

NativeEvent::NativeEvent(EventType _type, NativeEventParams p) : type(_type), params(p) {}

NativeEvent::~NativeEvent() {
    if (params == nullptr) {
        return;
    }
    switch (type) {
        case CursorEnter:
            delete reinterpret_cast<CursorEnterParams *>(params);
            break;
        case CursorExit:
            delete reinterpret_cast<CursorExitParams *>(params);
            break;
        case CursorMove:
            delete reinterpret_cast<CursorMoveParams *>(params);
            break;
        case LMouseDown:
            delete reinterpret_cast<LMouseDownParams *>(params);
            break;
        case LMouseUp:
            delete reinterpret_cast<LMouseUpParams *>(params);
            break;
        case RMouseDown:
            delete reinterpret_cast<RMouseDownParams *>(params);
            break;
        case RMouseUp:
            delete reinterpret_cast<RMouseUpParams *>(params);
            break;
        case DragBegin:
        case DragMove:
        case DragEnd:
            delete reinterpret_cast<MouseEventParams *>(params);
            break;
        case KeyDown:
            delete reinterpret_cast<KeyDownParams *>(params);
            break;
        case KeyUp:
            delete reinterpret_cast<KeyUpParams *>(params);
            break;
        case ViewResize:
            delete reinterpret_cast<ViewResize *>(params);
            break;
        case ScrollLeft:
        case ScrollRight:
        case ScrollUp:
        case ScrollDown:
            delete reinterpret_cast<ScrollParams *>(params);
            break;
        case WindowWillStartResize:
        case WindowWillResize:
            delete reinterpret_cast<WindowWillResize *>(params);
            break;
        case HasLoaded:
        case FocusGained:
        case FocusLost:
        case GesturePinch:
        case GesturePan:
        case GestureRotate:
        case AppActivate:
        case AppDeactivate:
        case WindowWillClose:
        case WindowHasResized:
        case WindowHasFinishedResize:
        case Unknown:
        default:
            break;
    }
    params = nullptr;
}

NativeEventEmitter::NativeEventEmitter() = default;

NativeEventEmitter::~NativeEventEmitter() = default;

void NativeEventEmitter::addReceiver(NativeEventProcessor *receiver) {
    if (receiver == nullptr) return;
    for (auto *r : receivers) {
        if (r == receiver) return;
    }
    receivers.push_back(receiver);
}

void NativeEventEmitter::removeReceiver(NativeEventProcessor *receiver) {
    if (receiver == nullptr) return;
    auto it = std::find(receivers.begin(), receivers.end(), receiver);
    if (it != receivers.end()) {
        receivers.erase(it);
    }
}

void NativeEventEmitter::setReciever(NativeEventProcessor *responder) {
    receivers.clear();
    if (responder != nullptr) {
        receivers.push_back(responder);
    }
}

bool NativeEventEmitter::hasReciever() const {
    return !receivers.empty();
}

void NativeEventEmitter::emit(NativeEventPtr event) {
    for (auto *r : receivers) {
        if (r != nullptr) {
            r->onRecieveEvent(event);
        }
    }
}

NativeEventProcessor::NativeEventProcessor() = default;

NativeEventProcessor::~NativeEventProcessor() = default;

}
