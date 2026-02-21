#include "NativePrivate/win/WinEvent.h"

namespace OmegaWTK::Native {
NativeEventPtr button_event_to_native_event(NativeEvent::EventType event_type,LPPOINT pt){
    NativeEventParams params;
    switch (event_type) {
    case NativeEvent::CursorEnter : {
        params = new CursorEnterParams();
        break;
    }
    case NativeEvent::CursorExit : {
        params = new CursorExitParams();
        break;
    };
    case NativeEvent::LMouseDown : {
        params = new LMouseDownParams();
        break;
    };
    case NativeEvent::LMouseUp : {
        params = new LMouseUpParams();
        break;
    }
    default : {
        params = nullptr;
        break;
    }
    }
    return (NativeEventPtr)new NativeEvent(event_type,params);
};

NativeEventPtr scroll_event_to_native_event(NativeEvent::EventType event_type,float deltaX,float deltaY){
    switch(event_type){
        case NativeEvent::ScrollLeft:
        case NativeEvent::ScrollRight:
        case NativeEvent::ScrollUp:
        case NativeEvent::ScrollDown:
            return NativeEventPtr(new NativeEvent(event_type,new ScrollParams{deltaX,deltaY}));
        default:
            return NativeEventPtr(new NativeEvent(NativeEvent::Unknown,nullptr));
    }
}
}
