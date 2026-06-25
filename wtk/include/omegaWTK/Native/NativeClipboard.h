#ifndef OMEGAWTK_NATIVE_NATIVECLIPBOARD_H
#define OMEGAWTK_NATIVE_NATIVECLIPBOARD_H

#include "omegaWTK/Core/Core.h"

namespace OmegaWTK::Native {

    /// The kinds of payload the system clipboard can hold. Used as the
    /// query key for `NativeClipboard::hasType`. v0 read/write coverage
    /// is `PlainText` and `FilePaths`; `HTML` and `Image` are queryable
    /// via `hasType` (so a consumer can decide whether richer handling
    /// is worthwhile) but have no get/set accessor yet — those land as a
    /// follow-up if a widget needs them.
    enum class ClipboardDataType : int {
        PlainText,
        HTML,
        Image,
        FilePaths
    };

    /// Read and write the system clipboard (the shared Ctrl+C / Cmd+C
    /// buffer, not a per-window selection).
    ///
    /// Threading: every method runs on the **main / UI thread**, the
    /// same contract as the rest of the Native layer. The read accessors
    /// (`getText` / `getFilePaths`) are synchronous — on backends whose
    /// clipboard transfer is asynchronous (GTK's X11/Wayland selection
    /// protocol) they pump a nested run-loop until the owning process
    /// replies, so they must not be called off the UI thread.
    ///
    /// Ownership: `get_native_clipboard()` returns a shared handle to a
    /// thin, stateless wrapper over the process-global system clipboard.
    /// The handle owns no clipboard data — the OS does. A copied payload
    /// outlives the handle (and, where the platform's clipboard manager
    /// persists it, the process).
    INTERFACE NativeClipboard {
    public:
        /// True iff the clipboard currently offers `type`. A read of a
        /// type that is not present returns empty (`getText`) / an empty
        /// vector (`getFilePaths`), so calling `hasType` first is only
        /// needed when the *kind* of available content drives behavior.
        virtual bool hasType(ClipboardDataType type) const = 0;

        /// The clipboard's plain-text content as UTF-8, or an empty
        /// string when no text is available.
        virtual OmegaCommon::String getText() const = 0;

        /// Replace the clipboard's content with `text` (UTF-8). This
        /// clears whatever was on the clipboard first.
        virtual void setText(const OmegaCommon::String & text) = 0;

        /// The clipboard's file list as local filesystem paths, or an
        /// empty vector when no file list is available. Backends that
        /// represent the list as `file://` URIs convert them to local
        /// paths here.
        virtual OmegaCommon::Vector<OmegaCommon::FS::Path> getFilePaths() const = 0;

        /// Replace the clipboard's content with the file list `paths`.
        /// This clears whatever was on the clipboard first.
        virtual void setFilePaths(const OmegaCommon::Vector<OmegaCommon::FS::Path> & paths) = 0;

        /// Empty the clipboard.
        virtual void clear() = 0;

        virtual ~NativeClipboard() = default;
    };
    typedef SharedHandle<NativeClipboard> NativeClipboardPtr;

    /// Obtain the system clipboard. Returns a handle to a process-wide
    /// singleton wrapper; repeated calls share the same instance. The
    /// backend resolves the OS clipboard lazily on each access, so it is
    /// safe to hold the handle across display teardown — methods become
    /// no-ops (and reads return empty) when no display is available.
    OMEGAWTK_EXPORT NativeClipboardPtr get_native_clipboard();

}

#endif
