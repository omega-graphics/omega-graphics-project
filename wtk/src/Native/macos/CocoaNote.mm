#include "NativePrivate/macos/CocoaUtils.h"
#include "omegaWTK/Native/NativeNote.h"

#import <UserNotifications/UserNotifications.h>

namespace OmegaWTK::Native::Cocoa {

    class CocoaNoteCenter : public NativeNoteCenter {
        UNUserNotificationCenter *notificationCenter;
    public:
        CocoaNoteCenter():notificationCenter([UNUserNotificationCenter currentNotificationCenter]){

        }
        void sendNativeNote(NativeNote &note) override {
            UNMutableNotificationContent *noteContent = [[UNMutableNotificationContent alloc] init];
            noteContent.title = common_string_to_ns_string(note.title);
            noteContent.body = common_string_to_ns_string(note.body);
            UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:@"" content:noteContent trigger:nil];
            [notificationCenter addNotificationRequest:request withCompletionHandler:nil];
            [noteContent release];
        };
        ~CocoaNoteCenter() = default;
    };

    class CocoaNote {
        __strong UNNotification *notification;
    public:
        
    };

}

namespace OmegaWTK::Native {
    NNCP make_native_note_center(){
        return (NNCP) make<Cocoa::CocoaNoteCenter>();
    }
}
