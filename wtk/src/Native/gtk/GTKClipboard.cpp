#include "omegaWTK/Native/NativeClipboard.h"

#include <gtk/gtk.h>

#include <memory>

namespace OmegaWTK::Native::GTK {

/// GtkClipboard-backed implementation of the system clipboard. Wraps the
/// shared `GDK_SELECTION_CLIPBOARD` clipboard (the Ctrl+C / Ctrl+V
/// buffer, not the X11 PRIMARY middle-click selection).
///
/// The wrapper is stateless: it resolves the GtkClipboard lazily on each
/// call so a handle obtained early (or held across display teardown)
/// stays valid — when there is no default display every method degrades
/// to a no-op and the reads return empty, per NativeClipboard.h.
class GTKClipboard : public NativeClipboard {
public:
    GTKClipboard() = default;
    ~GTKClipboard() override = default;

    bool hasType(ClipboardDataType type) const override {
        GtkClipboard *cb = clipboard();
        if(cb == nullptr){
            return false;
        }
        switch(type){
            case ClipboardDataType::PlainText:
                return gtk_clipboard_wait_is_text_available(cb) == TRUE;
            case ClipboardDataType::HTML:
                // No dedicated wait_is_html_available; probe the target.
                return gtk_clipboard_wait_is_target_available(
                    cb, gdk_atom_intern_static_string("text/html")) == TRUE;
            case ClipboardDataType::Image:
                return gtk_clipboard_wait_is_image_available(cb) == TRUE;
            case ClipboardDataType::FilePaths:
                return gtk_clipboard_wait_is_uris_available(cb) == TRUE;
        }
        return false;
    }

    OmegaCommon::String getText() const override {
        GtkClipboard *cb = clipboard();
        if(cb == nullptr){
            return {};
        }
        // wait_for_text pumps a nested loop until the owner replies;
        // returns a freshly-allocated string (or null when no text).
        gchar *text = gtk_clipboard_wait_for_text(cb);
        if(text == nullptr){
            return {};
        }
        OmegaCommon::String out{text};
        g_free(text);
        return out;
    }

    void setText(const OmegaCommon::String & text) override {
        GtkClipboard *cb = clipboard();
        if(cb == nullptr){
            return;
        }
        // set_text copies the buffer, so the std::string need not outlive
        // the call. -1 lets GTK measure the NUL-terminated length, but we
        // pass the explicit byte count to stay correct for embedded NULs.
        gtk_clipboard_set_text(cb, text.c_str(), (gint)text.size());
    }

    OmegaCommon::Vector<OmegaCommon::FS::Path> getFilePaths() const override {
        OmegaCommon::Vector<OmegaCommon::FS::Path> out;
        GtkClipboard *cb = clipboard();
        if(cb == nullptr){
            return out;
        }
        // NULL-terminated array of file:// URIs (or null when none).
        gchar **uris = gtk_clipboard_wait_for_uris(cb);
        if(uris == nullptr){
            return out;
        }
        for(gchar **u = uris; *u != nullptr; ++u){
            gchar *filename = g_filename_from_uri(*u, nullptr, nullptr);
            if(filename != nullptr){
                out.push_back(OmegaCommon::FS::Path(filename));
                g_free(filename);
            }
        }
        g_strfreev(uris);
        return out;
    }

    void setFilePaths(const OmegaCommon::Vector<OmegaCommon::FS::Path> & paths) override {
        GtkClipboard *cb = clipboard();
        if(cb == nullptr){
            return;
        }

        // Build a NULL-terminated, owned array of file:// URIs. The
        // clipboard's get-callback hands these to a requesting paste; the
        // clear-callback frees them when ownership is lost. Ownership of
        // the array transfers to the clipboard on a successful
        // set_with_data; on failure we free it ourselves below.
        GPtrArray *uriArray = g_ptr_array_new();
        for(const auto & path : paths){
            // str() is non-const and returns a reference, so read it off a
            // local copy rather than const_cast'ing the iterated element.
            OmegaCommon::FS::Path p = path;
            OmegaCommon::String s = p.str();
            gchar *uri = g_filename_to_uri(s.c_str(), nullptr, nullptr);
            if(uri != nullptr){
                g_ptr_array_add(uriArray, uri);
            }
        }
        g_ptr_array_add(uriArray, nullptr);   // NULL terminator
        // Take the raw gchar** out of the GPtrArray (keep the buffer).
        gchar **uris = (gchar **)g_ptr_array_free(uriArray, FALSE);

        static const GtkTargetEntry kUriTargets[] = {
            { const_cast<gchar *>("text/uri-list"), 0, 0 }
        };

        gboolean ok = gtk_clipboard_set_with_data(
            cb, kUriTargets, G_N_ELEMENTS(kUriTargets),
            &GTKClipboard::uriGetFunc, &GTKClipboard::uriClearFunc, uris);

        if(ok != TRUE){
            // set_with_data refused ownership — the clear-callback will
            // never run, so free our buffer here to avoid a leak.
            g_strfreev(uris);
        }
    }

    void clear() override {
        GtkClipboard *cb = clipboard();
        if(cb == nullptr){
            return;
        }
        gtk_clipboard_clear(cb);
    }

private:
    /// Resolve the shared system clipboard, or null when no display is up
    /// (gtk_clipboard_get asserts on a null default display, so guard it).
    static GtkClipboard * clipboard() {
        if(gdk_display_get_default() == nullptr){
            return nullptr;
        }
        return gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    }

    /// Serve the stored file:// URI list to a requesting paste.
    static void uriGetFunc(GtkClipboard * /*cb*/, GtkSelectionData *selection,
                           guint /*info*/, gpointer data){
        gchar **uris = static_cast<gchar **>(data);
        if(uris != nullptr){
            gtk_selection_data_set_uris(selection, uris);
        }
    }

    /// Free the URI list when the clipboard owner changes (someone else
    /// copied, or the contents were cleared).
    static void uriClearFunc(GtkClipboard * /*cb*/, gpointer data){
        gchar **uris = static_cast<gchar **>(data);
        if(uris != nullptr){
            g_strfreev(uris);
        }
    }
};

}

namespace OmegaWTK::Native {
    NativeClipboardPtr get_native_clipboard(){
        // Process-wide singleton: the wrapper is stateless and the OS
        // clipboard it fronts is itself a single shared resource.
        static NativeClipboardPtr instance = std::make_shared<GTK::GTKClipboard>();
        return instance;
    }
}
