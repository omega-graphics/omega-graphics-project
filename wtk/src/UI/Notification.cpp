#include "omegaWTK/UI/Notification.h"

namespace OmegaWTK {

    struct NotificationCenter::DelegateBridge : public Native::NativeNoteCenterDelegate {
        NotificationDelegate *user = nullptr;
        void onNoteActivated(const OmegaCommon::String & noteId) override {
            if(user) user->onNotificationActivated(noteId);
        }
        void onNoteAction(const OmegaCommon::String & noteId,
                          const OmegaCommon::String & actionId) override {
            if(user) user->onNotificationAction(noteId, actionId);
        }
    };

    NotificationCenter::NotificationCenter()
        : nativeNoteCenter(Native::make_native_note_center()),
          bridge(std::make_unique<DelegateBridge>()) {
        nativeNoteCenter->setDelegate(bridge.get());
    }

    NotificationCenter::~NotificationCenter() {
        if(nativeNoteCenter) nativeNoteCenter->setDelegate(nullptr);
    }

    void NotificationCenter::requestPermission(std::function<void(bool)> callback) {
        nativeNoteCenter->requestPermission([cb = std::move(callback)](Native::NativeNotePermission p){
            if(cb) cb(p == Native::NativeNotePermission::Authorized
                   || p == Native::NativeNotePermission::Provisional);
        });
    }

    bool NotificationCenter::isAuthorized() const {
        auto p = nativeNoteCenter->currentPermission();
        return p == Native::NativeNotePermission::Authorized
            || p == Native::NativeNotePermission::Provisional;
    }

    void NotificationCenter::send(NotificationDesc desc){
        Native::NativeNote note;
        note.title = desc.title;
        note.body = desc.body;
        note.identifier = desc.identifier;
        note.categoryIdentifier = desc.categoryIdentifier;
        note.delaySeconds = desc.delaySeconds;
        nativeNoteCenter->sendNativeNote(note);
    }

    void NotificationCenter::remove(const OmegaCommon::String & identifier) {
        nativeNoteCenter->removeNote(identifier);
    }

    void NotificationCenter::removeAll() {
        nativeNoteCenter->removeAllNotes();
    }

    void NotificationCenter::registerCategories(const OmegaCommon::Vector<NotificationCategory> & categories) {
        OmegaCommon::Vector<Native::NativeNoteCategory> nativeCats;
        nativeCats.reserve(categories.size());
        for(auto & cat : categories) {
            Native::NativeNoteCategory nc;
            nc.identifier = cat.identifier;
            nc.actions.reserve(cat.actions.size());
            for(auto & a : cat.actions) {
                Native::NativeNoteAction na;
                na.identifier = a.identifier;
                na.title = a.title;
                na.destructive = a.destructive;
                nc.actions.push_back(std::move(na));
            }
            nativeCats.push_back(std::move(nc));
        }
        nativeNoteCenter->registerCategories(nativeCats);
    }

    void NotificationCenter::setDelegate(NotificationDelegate *delegate) {
        bridge->user = delegate;
    }
};
