#include "NativePrivate/win/WinEvent.h"
#include <windowsx.h>

namespace OmegaWTK::Native {

static void fill_modifier_flags(ModifierFlags &m) {
    m.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    m.control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    m.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    m.meta = (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;
    m.capsLock = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
}

NativeEventPtr button_event_to_native_event(NativeEvent::EventType event_type, LPPOINT pt, HWND hwnd, float dpiScale) {
    if (pt == nullptr) {
        return NativeEventPtr(new NativeEvent(event_type, nullptr));
    }
    // pt is physical pixels; widget hit-test wants logical units.
    // ClientToScreen still gets the unscaled POINT — Win32's screen
    // space is also pixels — and the resulting screen coord is then
    // scaled to logical units too so callers see a consistent space.
    const float invScale = dpiScale > 0.f ? (1.f / dpiScale) : 1.f;
    Composition::Point2D clientPos;
    clientPos.x = static_cast<float>(pt->x) * invScale;
    clientPos.y = static_cast<float>(pt->y) * invScale;
    Composition::Point2D screenPos = clientPos;
    if (hwnd != nullptr) {
        POINT screenPt = *pt;
        if (ClientToScreen(hwnd, &screenPt)) {
            screenPos.x = static_cast<float>(screenPt.x) * invScale;
            screenPos.y = static_cast<float>(screenPt.y) * invScale;
        }
    }

    NativeEventParams params = nullptr;
    switch (event_type) {
        case NativeEvent::CursorEnter: {
            auto *p = new CursorEnterParams();
            p->position = clientPos;
            params = p;
            break;
        }
        case NativeEvent::CursorExit: {
            auto *p = new CursorExitParams();
            p->position = clientPos;
            params = p;
            break;
        }
        case NativeEvent::CursorMove: {
            auto *p = new CursorMoveParams();
            p->position = clientPos;
            p->screenPosition = screenPos;
            fill_modifier_flags(p->modifiers);
            params = p;
            break;
        }
        case NativeEvent::LMouseDown: {
            auto *p = new LMouseDownParams();
            p->position = clientPos;
            p->screenPosition = screenPos;
            fill_modifier_flags(p->modifiers);
            p->clickCount = 1;
            params = p;
            break;
        }
        case NativeEvent::LMouseUp: {
            auto *p = new LMouseUpParams();
            p->position = clientPos;
            p->screenPosition = screenPos;
            fill_modifier_flags(p->modifiers);
            p->clickCount = 1;
            params = p;
            break;
        }
        case NativeEvent::RMouseDown: {
            auto *p = new RMouseDownParams();
            p->position = clientPos;
            p->screenPosition = screenPos;
            fill_modifier_flags(p->modifiers);
            p->clickCount = 1;
            params = p;
            break;
        }
        case NativeEvent::RMouseUp: {
            auto *p = new RMouseUpParams();
            p->position = clientPos;
            p->screenPosition = screenPos;
            fill_modifier_flags(p->modifiers);
            p->clickCount = 1;
            params = p;
            break;
        }
        default:
            params = nullptr;
            break;
    }
    return NativeEventPtr(new NativeEvent(event_type, params));
}

NativeEventPtr scroll_event_to_native_event(NativeEvent::EventType event_type, float deltaX, float deltaY) {
    switch (event_type) {
        case NativeEvent::ScrollLeft:
        case NativeEvent::ScrollRight:
        case NativeEvent::ScrollUp:
        case NativeEvent::ScrollDown:
            return NativeEventPtr(new NativeEvent(event_type, new ScrollParams{deltaX, deltaY, Composition::Point2D{0.f,0.f}}));
        default:
            return NativeEventPtr(new NativeEvent(NativeEvent::Unknown, nullptr));
    }
}

}
