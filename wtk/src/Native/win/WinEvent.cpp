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

// Map a Win32 virtual-key code to the platform-independent KeyCode enum.
// Contiguous VK / KeyCode ranges (letters, digits, function keys) map by
// offset; the rest are an explicit switch. Generic VK_SHIFT/CONTROL/MENU
// (no left/right distinction) resolve to the Left variant, matching how the
// other backends collapse them.
static KeyCode key_code_from_vk(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<KeyCode>(static_cast<int>(KeyCode::A) + static_cast<int>(vk - 'A'));
    if (vk >= '0' && vk <= '9')
        return static_cast<KeyCode>(static_cast<int>(KeyCode::Num0) + static_cast<int>(vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F15)
        return static_cast<KeyCode>(static_cast<int>(KeyCode::F1) + static_cast<int>(vk - VK_F1));
    switch (vk) {
        case VK_ESCAPE:   return KeyCode::Escape;
        case VK_TAB:      return KeyCode::Tab;
        case VK_CAPITAL:  return KeyCode::CapsLock;
        case VK_SPACE:    return KeyCode::Space;
        case VK_RETURN:   return KeyCode::Enter;
        case VK_BACK:     return KeyCode::Backspace;
        case VK_DELETE:   return KeyCode::Delete;
        case VK_LSHIFT:   return KeyCode::LeftShift;
        case VK_RSHIFT:   return KeyCode::RightShift;
        case VK_SHIFT:    return KeyCode::LeftShift;
        case VK_LCONTROL: return KeyCode::LeftControl;
        case VK_RCONTROL: return KeyCode::RightControl;
        case VK_CONTROL:  return KeyCode::LeftControl;
        case VK_LMENU:    return KeyCode::LeftAlt;
        case VK_RMENU:    return KeyCode::RightAlt;
        case VK_MENU:     return KeyCode::LeftAlt;
        case VK_LWIN:     return KeyCode::LeftMeta;
        case VK_RWIN:     return KeyCode::RightMeta;
        case VK_UP:       return KeyCode::ArrowUp;
        case VK_DOWN:     return KeyCode::ArrowDown;
        case VK_LEFT:     return KeyCode::ArrowLeft;
        case VK_RIGHT:    return KeyCode::ArrowRight;
        case VK_HOME:     return KeyCode::Home;
        case VK_END:      return KeyCode::End;
        case VK_PRIOR:    return KeyCode::PageUp;
        case VK_NEXT:     return KeyCode::PageDown;
        default:          return KeyCode::Unknown;
    }
}

// Resolve the Unicode code point a key produces under the current keyboard
// state (Shift / CapsLock applied). Returns 0 for non-printing keys (arrows,
// F-keys, etc.), for which the caller relies on `code`. Note: ToUnicode()
// consumes any pending dead-key state; a v0 single-line field with no IME /
// dead-key composition is unaffected, but full dead-key input is a follow-up.
static OmegaCommon::Unicode32Char char_from_vk(WPARAM vk, LPARAM lParam) {
    BYTE keyState[256];
    if (!GetKeyboardState(keyState)) {
        return 0;
    }
    const UINT scanCode = static_cast<UINT>((lParam >> 16) & 0xFF);
    WCHAR buf[4] = {};
    const int n = ToUnicode(static_cast<UINT>(vk), scanCode, keyState, buf,
                            static_cast<int>(sizeof(buf) / sizeof(buf[0])), 0);
    if (n == 1) {
        return static_cast<OmegaCommon::Unicode32Char>(buf[0]);
    }
    return 0;
}

NativeEventPtr key_event_to_native_event(NativeEvent::EventType event_type, WPARAM vk, LPARAM lParam) {
    if (event_type == NativeEvent::KeyDown) {
        auto *p = new KeyDownParams();
        p->code = key_code_from_vk(vk);
        p->key = char_from_vk(vk, lParam);
        fill_modifier_flags(p->modifiers);
        // lParam bit 30 = previous key state (1 => the key was already down,
        // i.e. this is an auto-repeat).
        p->isRepeat = (lParam & (1 << 30)) != 0;
        return NativeEventPtr(new NativeEvent(event_type, p));
    }
    auto *p = new KeyUpParams();
    p->code = key_code_from_vk(vk);
    p->key = char_from_vk(vk, lParam);
    fill_modifier_flags(p->modifiers);
    return NativeEventPtr(new NativeEvent(event_type, p));
}

}
