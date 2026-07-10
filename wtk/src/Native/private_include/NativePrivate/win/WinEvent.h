#include <Windows.h>
#include "omegaWTK/Native/NativeEvent.h"

#ifndef OMEGAWTK_NATIVE_WIN_WINEVENT_H
#define OMEGAWTK_NATIVE_WIN_WINEVENT_H

namespace OmegaWTK::Native {

/// pt is in client coordinates (physical pixels). If hwnd is non-null,
/// screenPosition is filled via ClientToScreen. dpiScale = currentDpi/96.f
/// — the emitted position/screenPosition are divided by it so the
/// widget tree's hit test (which works in logical DPI-independent
/// units) lands on the right widget on non-96-DPI displays.
NativeEventPtr button_event_to_native_event(NativeEvent::EventType event_type, LPPOINT pt, HWND hwnd = nullptr, float dpiScale = 1.f);
NativeEventPtr scroll_event_to_native_event(NativeEvent::EventType event_type,float deltaX,float deltaY);

/// Build a KeyDown / KeyUp NativeEvent from a WM_KEYDOWN / WM_KEYUP.
/// `vk` is the virtual-key code (wParam); `lParam` carries the scan code
/// (bits 16-23) and the repeat/previous-state flag (bit 30). The virtual
/// key maps to `KeyCode`, and for a printable key the current keyboard
/// state is resolved to a Unicode code point via ToUnicode() so the one
/// event carries both `code` and `key` (matching the macOS/GTK model).
NativeEventPtr key_event_to_native_event(NativeEvent::EventType event_type, WPARAM vk, LPARAM lParam);

};

#endif
