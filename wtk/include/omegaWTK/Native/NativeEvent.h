#include "omegaWTK/Core/Core.h"
#include <functional>
#include <cstdint>

#ifndef OMEGAWTK_NATIVE_NATIVEEVENT_H
#define OMEGAWTK_NATIVE_NATIVEEVENT_H

namespace OmegaWTK {
namespace Native {

typedef void* NativeEventParams;

// --- Modifier and event param structs (defined before NativeEvent for use in EventType) ---

struct OMEGAWTK_EXPORT ModifierFlags {
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool meta = false;
    bool capsLock = false;
};

struct OMEGAWTK_EXPORT MouseEventParams {
    Core::Position position;
    Core::Position screenPosition;
    ModifierFlags modifiers;
    unsigned clickCount = 1;
};

struct OMEGAWTK_EXPORT CursorMoveParams {
    Core::Position position;
    Core::Position screenPosition;
    ModifierFlags modifiers;
};

struct OMEGAWTK_EXPORT CursorEnterParams {
    Core::Position position;
};

struct OMEGAWTK_EXPORT CursorExitParams {
    Core::Position position;
};

struct OMEGAWTK_EXPORT LMouseDownParams : MouseEventParams {};
struct OMEGAWTK_EXPORT LMouseUpParams   : MouseEventParams {};
struct OMEGAWTK_EXPORT RMouseDownParams : MouseEventParams {};
struct OMEGAWTK_EXPORT RMouseUpParams   : MouseEventParams {};

enum class OMEGAWTK_EXPORT KeyCode : int {
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5,
    Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Escape, Tab, CapsLock, Space, Enter, Backspace, Delete,
    LeftShift, RightShift, LeftControl, RightControl,
    LeftAlt, RightAlt, LeftMeta, RightMeta,
    ArrowUp, ArrowDown, ArrowLeft, ArrowRight,
    Home, End, PageUp, PageDown,
    Unknown
};

struct OMEGAWTK_EXPORT KeyDownParams {
    KeyCode code = KeyCode::Unknown;
    OmegaWTK::Unicode32Char key = 0;
    ModifierFlags modifiers;
    bool isRepeat = false;
};

struct OMEGAWTK_EXPORT KeyUpParams {
    KeyCode code = KeyCode::Unknown;
    OmegaWTK::Unicode32Char key = 0;
    ModifierFlags modifiers;
};

struct OMEGAWTK_EXPORT ViewHasLoaded {};

struct OMEGAWTK_EXPORT ViewResize {
    Core::Rect rect;
};

struct OMEGAWTK_EXPORT ScrollParams {
    float deltaX;
    float deltaY;
};

struct OMEGAWTK_EXPORT WindowWillResize {
    Core::Rect rect;
    std::uint64_t generation;
    WindowWillResize(Core::Rect rect, std::uint64_t generation = 0);
};

// --- NativeEvent ---

class OMEGAWTK_EXPORT NativeEvent {
public:
    typedef enum : OPT_PARAM {
        Unknown,
        HasLoaded,
        CursorEnter,
        CursorExit,
        CursorMove,
        LMouseDown,
        LMouseUp,
        RMouseDown,
        RMouseUp,
        DragBegin,
        DragMove,
        DragEnd,
        KeyDown,
        KeyUp,
        ViewResize,
        ScrollLeft,
        ScrollRight,
        ScrollUp,
        ScrollDown,
        FocusGained,
        FocusLost,
        GesturePinch,
        GesturePan,
        GestureRotate,
        AppActivate,
        AppDeactivate,
        WindowWillClose,
        WindowWillStartResize,
        WindowWillResize,
        WindowHasResized,
        WindowHasFinishedResize
    } EventType;
    EventType type;
    NativeEventParams params;
public:
    NativeEvent(EventType _type, NativeEventParams params);
    ~NativeEvent();
};

typedef SharedHandle<NativeEvent> NativeEventPtr;

// --- Event processor and emitter ---

class NativeEventProcessor;

class OMEGAWTK_EXPORT NativeEventEmitter {
    OmegaCommon::Vector<NativeEventProcessor *> receivers;
    friend class NativeEventProcessor;
public:
    NativeEventEmitter();
    void addReceiver(NativeEventProcessor *receiver);
    void removeReceiver(NativeEventProcessor *receiver);
    void setReciever(NativeEventProcessor *responder);
    bool hasReciever() const;
    void emit(NativeEventPtr event);
    ~NativeEventEmitter();
};

class OMEGAWTK_EXPORT NativeEventProcessor {
public:
    virtual void onRecieveEvent(NativeEventPtr event) = 0;
    NativeEventProcessor();
    virtual ~NativeEventProcessor();
};

}
}

#endif
