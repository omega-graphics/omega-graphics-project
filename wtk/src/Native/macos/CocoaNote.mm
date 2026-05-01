#include "NativePrivate/macos/CocoaUtils.h"
#include "omegaWTK/Native/NativeNote.h"

#import <UserNotifications/UserNotifications.h>
#import <Foundation/Foundation.h>

#include <atomic>

@class OmegaWTKUNDelegate;

namespace OmegaWTK::Native::Cocoa {

    class CocoaNoteCenter;

    static OmegaCommon::String ns_to_common(NSString *s) {
        OmegaCommon::String out;
        if(s) ns_string_to_common_string(s, out);
        return out;
    }

    static OmegaCommon::String make_auto_identifier() {
        // UUIDs are stable, unique, and easy to round-trip through NSString.
        NSString *uuid = [[NSUUID UUID] UUIDString];
        return ns_to_common(uuid);
    }

    class CocoaNoteCenter : public NativeNoteCenter {
        UNUserNotificationCenter *notificationCenter;
        OmegaWTKUNDelegate *cocoaDelegate;
        NativeNoteCenterDelegate *userDelegate = nullptr;
        std::atomic<NativeNotePermission> cachedPermission{NativeNotePermission::NotDetermined};

        void refreshPermissionCache(){
            // Snapshot current settings into the cache without blocking the caller.
            [notificationCenter getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings * _Nonnull settings) {
                NativeNotePermission p;
                switch(settings.authorizationStatus) {
                    case UNAuthorizationStatusAuthorized:    p = NativeNotePermission::Authorized; break;
                    case UNAuthorizationStatusDenied:        p = NativeNotePermission::Denied; break;
                    case UNAuthorizationStatusProvisional:   p = NativeNotePermission::Provisional; break;
                    case UNAuthorizationStatusNotDetermined:
                    default:                                 p = NativeNotePermission::NotDetermined; break;
                }
                cachedPermission.store(p);
            }];
        }
    public:
        CocoaNoteCenter();
        ~CocoaNoteCenter() override;

        void requestPermission(std::function<void(NativeNotePermission)> callback) override {
            UNAuthorizationOptions opts = UNAuthorizationOptionAlert
                                        | UNAuthorizationOptionSound
                                        | UNAuthorizationOptionBadge;
            // Move the std::function into a __block so the Objective-C block can call it.
            __block std::function<void(NativeNotePermission)> cb = std::move(callback);
            [notificationCenter requestAuthorizationWithOptions:opts
                                              completionHandler:^(BOOL granted, NSError * _Nullable error) {
                NativeNotePermission p = granted ? NativeNotePermission::Authorized
                                                 : NativeNotePermission::Denied;
                cachedPermission.store(p);
                if(cb) cb(p);
            }];
        }

        NativeNotePermission currentPermission() const override {
            return cachedPermission.load();
        }

        void sendNativeNote(NativeNote &note) override {
            UNMutableNotificationContent *noteContent = [[UNMutableNotificationContent alloc] init];
            noteContent.title = common_string_to_ns_string(note.title);
            noteContent.body  = common_string_to_ns_string(note.body);
            if(!note.categoryIdentifier.empty()) {
                noteContent.categoryIdentifier = common_string_to_ns_string(note.categoryIdentifier);
            }

            OmegaCommon::String idStr = note.identifier.empty()
                                      ? make_auto_identifier()
                                      : note.identifier;
            NSString *identifier = common_string_to_ns_string(idStr);

            UNNotificationTrigger *trigger = nil;
            if(note.delaySeconds > 0.f) {
                // UN requires interval >= 1 for non-repeating triggers.
                NSTimeInterval interval = MAX((NSTimeInterval)note.delaySeconds, 1.0);
                trigger = [UNTimeIntervalNotificationTrigger triggerWithTimeInterval:interval
                                                                             repeats:NO];
            }

            UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:identifier
                                                                                  content:noteContent
                                                                                  trigger:trigger];
            [notificationCenter addNotificationRequest:request withCompletionHandler:nil];
            [noteContent release];
        }

        void removeNote(const OmegaCommon::String & identifier) override {
            NSString *idStr = common_string_to_ns_string(identifier);
            NSArray<NSString *> *ids = @[ idStr ];
            [notificationCenter removePendingNotificationRequestsWithIdentifiers:ids];
            [notificationCenter removeDeliveredNotificationsWithIdentifiers:ids];
        }

        void removeAllNotes() override {
            [notificationCenter removeAllPendingNotificationRequests];
            [notificationCenter removeAllDeliveredNotifications];
        }

        void registerCategories(const OmegaCommon::Vector<NativeNoteCategory> & categories) override {
            NSMutableSet<UNNotificationCategory *> *set = [NSMutableSet setWithCapacity:categories.size()];
            for(auto & cat : categories) {
                NSMutableArray<UNNotificationAction *> *actions = [NSMutableArray arrayWithCapacity:cat.actions.size()];
                for(auto & a : cat.actions) {
                    UNNotificationActionOptions opts = UNNotificationActionOptionForeground;
                    if(a.destructive) opts |= UNNotificationActionOptionDestructive;
                    UNNotificationAction *action =
                        [UNNotificationAction actionWithIdentifier:common_string_to_ns_string(a.identifier)
                                                             title:common_string_to_ns_string(a.title)
                                                           options:opts];
                    [actions addObject:action];
                }
                UNNotificationCategory *uncat =
                    [UNNotificationCategory categoryWithIdentifier:common_string_to_ns_string(cat.identifier)
                                                           actions:actions
                                                 intentIdentifiers:@[]
                                                           options:UNNotificationCategoryOptionNone];
                [set addObject:uncat];
            }
            [notificationCenter setNotificationCategories:set];
        }

        void setDelegate(NativeNoteCenterDelegate * delegate) override {
            userDelegate = delegate;
        }

        void dispatchActivated(NSString *noteId) {
            if(userDelegate) userDelegate->onNoteActivated(ns_to_common(noteId));
        }

        void dispatchAction(NSString *noteId, NSString *actionId) {
            if(userDelegate) userDelegate->onNoteAction(ns_to_common(noteId), ns_to_common(actionId));
        }
    };

}

@interface OmegaWTKUNDelegate : NSObject<UNUserNotificationCenterDelegate> {
    @public
    OmegaWTK::Native::Cocoa::CocoaNoteCenter *owner;
}
@end

@implementation OmegaWTKUNDelegate

- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
    // Show the banner / play sound even when the app is in the foreground.
    completionHandler(UNNotificationPresentationOptionBanner
                    | UNNotificationPresentationOptionSound);
}

- (void)userNotificationCenter:(UNUserNotificationCenter *)center
didReceiveNotificationResponse:(UNNotificationResponse *)response
         withCompletionHandler:(void (^)(void))completionHandler {
    if(owner) {
        NSString *noteId = response.notification.request.identifier;
        NSString *actionId = response.actionIdentifier;
        if([actionId isEqualToString:UNNotificationDefaultActionIdentifier]) {
            owner->dispatchActivated(noteId);
        } else if(![actionId isEqualToString:UNNotificationDismissActionIdentifier]) {
            owner->dispatchAction(noteId, actionId);
        }
    }
    completionHandler();
}

@end

namespace OmegaWTK::Native::Cocoa {

    CocoaNoteCenter::CocoaNoteCenter()
        : notificationCenter([UNUserNotificationCenter currentNotificationCenter]) {
        cocoaDelegate = [[OmegaWTKUNDelegate alloc] init];
        cocoaDelegate->owner = this;
        notificationCenter.delegate = cocoaDelegate;
        refreshPermissionCache();
    }

    CocoaNoteCenter::~CocoaNoteCenter() {
        if(notificationCenter.delegate == cocoaDelegate) {
            notificationCenter.delegate = nil;
        }
        cocoaDelegate->owner = nullptr;
        [cocoaDelegate release];
    }

}

namespace OmegaWTK::Native {
    NNCP make_native_note_center(){
        return (NNCP) make<Cocoa::CocoaNoteCenter>();
    }
}
