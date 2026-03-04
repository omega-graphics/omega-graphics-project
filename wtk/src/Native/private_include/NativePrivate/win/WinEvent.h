#include <Windows.h>
#include "omegaWTK/Native/NativeEvent.h"

#ifndef OMEGAWTK_NATIVE_WIN_WINEVENT_H
#define OMEGAWTK_NATIVE_WIN_WINEVENT_H

namespace OmegaWTK::Native {

/// pt is in client coordinates. If hwnd is non-null, screenPosition is filled via ClientToScreen.
NativeEventPtr button_event_to_native_event(NativeEvent::EventType event_type, LPPOINT pt, HWND hwnd = nullptr);
NativeEventPtr scroll_event_to_native_event(NativeEvent::EventType event_type,float deltaX,float deltaY);

};

#endif
