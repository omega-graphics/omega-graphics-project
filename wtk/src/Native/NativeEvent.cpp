#include "omegaWTK/Native/NativeEvent.h"

namespace OmegaWTK::Native {

WindowWillResize::WindowWillResize(Core::Rect rect):rect(rect){};

NativeEvent::~NativeEvent(){
    if(params == nullptr){
        return;
    }
    switch(type){
        case CursorEnter:
            delete reinterpret_cast<CursorEnterParams *>(params);
            break;
        case CursorExit:
            delete reinterpret_cast<CursorExitParams *>(params);
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
        case KeyDown:
            delete reinterpret_cast<KeyDownParams *>(params);
            break;
        case KeyUp:
            delete reinterpret_cast<KeyUpParams *>(params);
            break;
        case ViewResize:
            delete reinterpret_cast<OmegaWTK::Native::ViewResize *>(params);
            break;
        case ScrollLeft:
        case ScrollRight:
        case ScrollUp:
        case ScrollDown:
            delete reinterpret_cast<OmegaWTK::Native::ScrollParams *>(params);
            break;
        case WindowWillResize:
            delete reinterpret_cast<OmegaWTK::Native::WindowWillResize *>(params);
            break;
        case HasLoaded:
        case WindowWillClose:
        case WindowHasResized:
        case WindowHasFinishedResize:
        case Unknown:
        default:
            break;
    }
    params = nullptr;
};

NativeEventEmitter::NativeEventEmitter():message_reciever(nullptr){

};

NativeEventEmitter::~NativeEventEmitter(){};

bool NativeEventEmitter::hasReciever(){
    return message_reciever != nullptr;
};

void NativeEventEmitter::setReciever(NativeEventProcessor *_responder){
    message_reciever = _responder;
};

void NativeEventEmitter::emit(NativeEventPtr event){
    if(hasReciever())
        message_reciever->onRecieveEvent(event);
};

NativeEventProcessor::NativeEventProcessor(){};


//void NativeEventProcessor::onRecieveEvent(NativeEventPtr event){
//
//};

NativeEventProcessor::~NativeEventProcessor(){
    
};

};
