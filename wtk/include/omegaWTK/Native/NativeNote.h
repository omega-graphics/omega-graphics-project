#include "omegaWTK/Core/Core.h"

#include <cstdint>
#include <functional>

#ifndef OMEGAWTK_NATIVE_NATIVENOTE_H
#define OMEGAWTK_NATIVE_NATIVENOTE_H

namespace OmegaWTK::Native {

    enum class NativeNotePermission : uint8_t {
        NotDetermined,
        Authorized,
        Denied,
        Provisional   // macOS: silent delivery to Notification Center
    };

    struct NativeNote {
        OmegaCommon::String title;
        OmegaCommon::String body;
        OmegaCommon::String identifier;          // Stable id; required for removeNote(). Empty = auto-generated.
        OmegaCommon::String categoryIdentifier;  // Matches a registered NativeNoteCategory
        float delaySeconds = 0.f;                // 0 = immediate
    };

    struct NativeNoteAction {
        OmegaCommon::String identifier;
        OmegaCommon::String title;
        bool destructive = false;
    };

    struct NativeNoteCategory {
        OmegaCommon::String identifier;
        OmegaCommon::Vector<NativeNoteAction> actions;
    };

    INTERFACE NativeNoteCenterDelegate {
    public:
        /// Called when the user taps/clicks the body of a notification.
        virtual void onNoteActivated(const OmegaCommon::String & noteId) {}
        /// Called when the user selects an action button on a notification.
        virtual void onNoteAction(const OmegaCommon::String & noteId,
                                  const OmegaCommon::String & actionId) {}
        virtual ~NativeNoteCenterDelegate() = default;
    };

    INTERFACE NativeNoteCenter {
    public:
        INTERFACE_METHOD void requestPermission(std::function<void(NativeNotePermission)> callback) ABSTRACT;
        INTERFACE_METHOD NativeNotePermission currentPermission() const ABSTRACT;

        INTERFACE_METHOD void sendNativeNote(NativeNote & note) ABSTRACT;
        INTERFACE_METHOD void removeNote(const OmegaCommon::String & identifier) ABSTRACT;
        INTERFACE_METHOD void removeAllNotes() ABSTRACT;

        INTERFACE_METHOD void registerCategories(
            const OmegaCommon::Vector<NativeNoteCategory> & categories) ABSTRACT;

        INTERFACE_METHOD void setDelegate(NativeNoteCenterDelegate * delegate) ABSTRACT;

        virtual ~NativeNoteCenter() = default;
    };

    using NNCP = SharedHandle<NativeNoteCenter>;

    NNCP make_native_note_center();

}

#endif
