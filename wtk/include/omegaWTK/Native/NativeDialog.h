#include "omegaWTK/Core/Core.h"
#include "NativeWindow.h"

#ifndef OMEGAWTK_NATIVE_NATIVEDIALOG_H
#define OMEGAWTK_NATIVE_NATIVEDIALOG_H

namespace OmegaWTK::Native {
    class NativeDialog {
        protected:
         NativeDialog(NWH parentWindow);
        NWH parentWindow;
        public:
        virtual ~NativeDialog() = default;
    };

    /// A named group of file extensions offered as a single choice in an
    /// open/save dialog. `extensions` are bare extensions without the dot
    /// (e.g. {"png","jpg"}); an empty `extensions` means "all files".
    struct FileFilter {
        OmegaCommon::String label;
        OmegaCommon::Vector<OmegaCommon::String> extensions;
    };

    class NativeFSDialog : public NativeDialog {
    protected:
        NativeFSDialog(NWH parentWindow);
    public:
        typedef enum : OPT_PARAM {
            Read,
            Write
        } Type;
        struct Descriptor {
            Type type;
            OmegaCommon::FS::Path openLocation;
            /// Restricts selectable files to the union of these filters'
            /// extensions. Empty == no restriction (all files).
            OmegaCommon::Vector<FileFilter> filters;
            /// Read dialogs only: allow selecting more than one file.
            /// Ignored for Write.
            bool allowMultiple = false;
        };
        static SharedHandle<NativeFSDialog> Create(const Descriptor & desc,NWH nativeWindow);
        /// Resolves with the chosen paths once the user dismisses the dialog.
        /// Empty when the dialog is cancelled. For single-select dialogs the
        /// vector holds at most one element.
        virtual OmegaCommon::Async<OmegaCommon::Vector<OmegaCommon::FS::Path>> getResult() = 0;
        virtual ~NativeFSDialog() = default;
    };

    /// A modal alert / confirmation dialog (OK/Cancel/Yes/No-style). Unlike
    /// NativeNoteDialog this reports which button the user pressed.
    class NativeAlertDialog : public NativeDialog {
    protected:
        NativeAlertDialog(NWH parentWindow);
    public:
        enum class Style : int { Info, Warning, Error };
        enum class Result : int { OK, Cancel, Yes, No };

        struct Descriptor {
            OmegaCommon::String title;
            OmegaCommon::String message;
            Style style = Style::Info;
            /// Buttons shown left-to-right. The clicked label is mapped to a
            /// Result by case-insensitive match against "OK"/"Cancel"/"Yes"/
            /// "No"; an unmatched label resolves to OK for the first button
            /// and Cancel otherwise. Empty == a single "OK" button.
            OmegaCommon::Vector<OmegaCommon::String> buttonLabels;
        };

        static SharedHandle<NativeAlertDialog> Create(const Descriptor & desc,NWH nativeWindow);
        /// Resolves with the pressed button once the user dismisses the dialog.
        virtual OmegaCommon::Async<Result> getResult() = 0;
        virtual ~NativeAlertDialog() = default;
    };

    class NativeNoteDialog : public NativeDialog {
    protected:
        NativeNoteDialog(NWH parentWindow);
    public:
        struct Descriptor {
            OmegaCommon::String title;
            OmegaCommon::String str;
        };
        static SharedHandle<NativeNoteDialog> Create(const Descriptor & desc,NWH nativeWindow);
        virtual ~NativeNoteDialog() = default;
    };


};

#endif
