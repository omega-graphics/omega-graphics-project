#include "omegaWTK/Core/Core.h"
#include "omega-common/unicode.h"
#include "omegaWTK/Composition/Geometry.h"
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
    Composition::Point2D position;
    Composition::Point2D screenPosition;
    ModifierFlags modifiers;
    unsigned clickCount = 1;
};

struct OMEGAWTK_EXPORT CursorMoveParams {
    Composition::Point2D position;
    Composition::Point2D screenPosition;
    ModifierFlags modifiers;
};

struct OMEGAWTK_EXPORT CursorEnterParams {
    Composition::Point2D position;
};

struct OMEGAWTK_EXPORT CursorExitParams {
    Composition::Point2D position;
};

struct OMEGAWTK_EXPORT LMouseDownParams : MouseEventParams {};
struct OMEGAWTK_EXPORT LMouseUpParams   : MouseEventParams {};
struct OMEGAWTK_EXPORT RMouseDownParams : MouseEventParams {};
struct OMEGAWTK_EXPORT RMouseUpParams   : MouseEventParams {};

enum class KeyCode : int {
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5,
    Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    F13, F14, F15,
    Escape, Tab, CapsLock, Space, Enter, Backspace, Delete,
    LeftShift, RightShift, LeftControl, RightControl,
    LeftAlt, RightAlt, LeftMeta, RightMeta,
    ArrowUp, ArrowDown, ArrowLeft, ArrowRight,
    Home, End, PageUp, PageDown,
    Unknown
};

struct OMEGAWTK_EXPORT KeyDownParams {
    KeyCode code = KeyCode::Unknown;
    OmegaCommon::Unicode32Char key = 0;
    ModifierFlags modifiers;
    bool isRepeat = false;
};

struct OMEGAWTK_EXPORT KeyUpParams {
    KeyCode code = KeyCode::Unknown;
    OmegaCommon::Unicode32Char key = 0;
    ModifierFlags modifiers;
};

struct OMEGAWTK_EXPORT ViewHasLoaded {};

struct OMEGAWTK_EXPORT ViewResize {
    Composition::Rect rect;
};

/// Scroll gesture phase (ScrollView-Interaction-Enhancements-Plan E5).
/// A discrete mouse wheel reports `None`; a trackpad reports the gesture
/// lifecycle so the ScrollView can tell user-driven scrolling from the
/// OS-generated momentum (fling) stream and avoid layering app-side
/// momentum on top of the OS's. Populated on macOS from NSEvent phase /
/// momentumPhase; Win32 / GTK send `None` today.
enum class ScrollPhase : std::uint8_t {
    None = 0,        ///< discrete wheel — no gesture phase
    Began,           ///< trackpad fingers-down, scroll starting
    Changed,         ///< trackpad user-driven scroll
    Ended,           ///< trackpad fingers lifted
    MomentumBegan,   ///< OS inertial fling starting
    Momentum,        ///< OS inertial fling decaying
    MomentumEnded    ///< OS inertial fling finished
};

struct OMEGAWTK_EXPORT ScrollParams {
    float deltaX;
    float deltaY;
    Composition::Point2D position;
    ScrollPhase phase = ScrollPhase::None;
};

struct OMEGAWTK_EXPORT WindowWillResize {
    Composition::Rect rect;
    std::uint64_t generation;
    WindowWillResize(Composition::Rect rect, std::uint64_t generation = 0);
};

struct OMEGAWTK_EXPORT WindowScaleFactorChangedParams {
    float oldScale = 1.f;
    float newScale = 1.f;
    /// Win32 WM_DPICHANGED suggests a new window rect that preserves
    /// physical size on the new monitor. macOS/Linux leave this empty.
    Core::Optional<Composition::Rect> suggestedRect;
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
        ScrollWheel,
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
        WindowHasFinishedResize,
        WindowScaleFactorChanged
    } EventType;
    EventType type;
    NativeEventParams params;
    /// Event-bubbling consumption flag (ScrollView-4.7-Integration-Plan
    /// V2). A handler that consumes this event sets `handled = true` to
    /// stop it propagating to ancestor views in `View::dispatchEvent`.
    /// A handler sets it ONLY for event types it actually consumes
    /// (Invariant A): e.g. a Button sets it for clicks but leaves it
    /// false for ScrollWheel so the wheel bubbles past to a ScrollView.
    bool handled = false;
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
