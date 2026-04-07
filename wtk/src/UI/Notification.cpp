#include "omegaWTK/UI/Notification.h"

namespace OmegaWTK {
    NotificationCenter::NotificationCenter()
        : nativeNoteCenter(Native::make_native_note_center()) {}

    void NotificationCenter::send(NotificationDesc desc){
        Native::NativeNote note;
        note.title = desc.title;
        note.body = desc.body;
        nativeNoteCenter->sendNativeNote(note);
    };
};