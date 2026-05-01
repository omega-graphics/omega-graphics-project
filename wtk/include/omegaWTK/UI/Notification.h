#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Native/NativeNote.h"

#include <functional>
#include <memory>

#ifndef OMEGAWTK_UI_NOTIFICATION_H
#define OMEGAWTK_UI_NOTIFICATION_H

namespace OmegaWTK {

    struct NotificationDesc {
        OmegaCommon::String title;
        OmegaCommon::String body;
        OmegaCommon::String identifier;       // Optional; auto-generated if empty
        OmegaCommon::String categoryIdentifier;
        float delaySeconds = 0.f;
    };

    struct NotificationAction {
        OmegaCommon::String identifier;
        OmegaCommon::String title;
        bool destructive = false;
    };

    struct NotificationCategory {
        OmegaCommon::String identifier;
        OmegaCommon::Vector<NotificationAction> actions;
    };

    INTERFACE NotificationDelegate {
    public:
        virtual void onNotificationActivated(const OmegaCommon::String & id) {}
        virtual void onNotificationAction(const OmegaCommon::String & id,
                                          const OmegaCommon::String & actionId) {}
        virtual ~NotificationDelegate() = default;
    };

    class OMEGAWTK_EXPORT NotificationCenter {
        Native::NNCP nativeNoteCenter;
        struct DelegateBridge;
        std::unique_ptr<DelegateBridge> bridge;
    public:
        OMEGACOMMON_CLASS("OmegaWTK.NotifcationCenter")

        NotificationCenter();
        ~NotificationCenter();

        /// Request user permission. The callback receives `true` if Authorized
        /// or Provisional, `false` for Denied.
        void requestPermission(std::function<void(bool granted)> callback);

        /// True iff the system has granted Authorized or Provisional permission.
        bool isAuthorized() const;

        void send(NotificationDesc desc);

        void remove(const OmegaCommon::String & identifier);
        void removeAll();

        void registerCategories(const OmegaCommon::Vector<NotificationCategory> & categories);

        /// Set or clear (nullptr) the delegate for activation/action callbacks.
        void setDelegate(NotificationDelegate * delegate);
    };
};

#endif
