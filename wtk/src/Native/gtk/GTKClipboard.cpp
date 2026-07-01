#include "omegaWTK/Native/NativeClipboard.h"

#include <gtk/gtk.h>

#include <memory>

namespace OmegaWTK::Native::GTK {


/// GdkClipboard-backed implementation. GTK 4 retired the synchronous
/// GtkClipboard (`wait_for_text`/`wait_for_uris`); GdkClipboard reads are
/// async-only. NativeClipboard's read accessors are synchronous by contract
/// (NativeClipboard.h: async backends "pump a nested run-loop until the owning
/// process replies"), so getText/getFilePaths fire the async read and iterate
/// the default GMainContext until the GAsyncResult callback lands. As before the
/// wrapper is stateless and resolves the clipboard lazily, degrading to no-op /
/// empty when no display is up.
class GTKClipboard : public NativeClipboard {
public:
    GTKClipboard() = default;
    ~GTKClipboard() override = default;

    bool hasType(ClipboardDataType type) const override {
        GdkClipboard *cb = clipboard();
        if(cb == nullptr) return false;
        GdkContentFormats *formats = gdk_clipboard_get_formats(cb); // borrowed
        if(formats == nullptr) return false;
        switch(type){
            case ClipboardDataType::PlainText:
                return gdk_content_formats_contain_gtype(formats, G_TYPE_STRING) == TRUE ||
                       gdk_content_formats_contain_mime_type(formats, "text/plain") == TRUE;
            case ClipboardDataType::HTML:
                return gdk_content_formats_contain_mime_type(formats, "text/html") == TRUE;
            case ClipboardDataType::Image:
                return gdk_content_formats_contain_gtype(formats, GDK_TYPE_TEXTURE) == TRUE ||
                       gdk_content_formats_contain_mime_type(formats, "image/png") == TRUE;
            case ClipboardDataType::FilePaths:
                return gdk_content_formats_contain_gtype(formats, GDK_TYPE_FILE_LIST) == TRUE ||
                       gdk_content_formats_contain_mime_type(formats, "text/uri-list") == TRUE;
        }
        return false;
    }

    OmegaCommon::String getText() const override {
        GdkClipboard *cb = clipboard();
        if(cb == nullptr) return {};
        TextReadState state;
        gdk_clipboard_read_text_async(cb, nullptr, &GTKClipboard::onTextRead, &state);
        pumpUntil(state.done);
        return state.text;
    }

    void setText(const OmegaCommon::String & text) override {
        GdkClipboard *cb = clipboard();
        if(cb == nullptr) return;
        // set_text copies the buffer; the std::string need not outlive the call.
        gdk_clipboard_set_text(cb, text.c_str());
    }

    OmegaCommon::Vector<OmegaCommon::FS::Path> getFilePaths() const override {
        GdkClipboard *cb = clipboard();
        if(cb == nullptr) return {};
        FileReadState state;
        gdk_clipboard_read_value_async(cb, GDK_TYPE_FILE_LIST, G_PRIORITY_DEFAULT,
            nullptr, &GTKClipboard::onFileRead, &state);
        pumpUntil(state.done);
        return state.paths;
    }

    void setFilePaths(const OmegaCommon::Vector<OmegaCommon::FS::Path> & paths) override {
        GdkClipboard *cb = clipboard();
        if(cb == nullptr) return;

        GSList *files = nullptr;
        for(const auto & path : paths){
            OmegaCommon::FS::Path p = path;             // str() is non-const
            OmegaCommon::String s = p.str();
            files = g_slist_append(files, g_file_new_for_path(s.c_str()));
        }
        GdkFileList *fileList = gdk_file_list_new_from_list(files);
        g_slist_free_full(files, g_object_unref);

        GValue value = G_VALUE_INIT;
        g_value_init(&value, GDK_TYPE_FILE_LIST);
        g_value_take_boxed(&value, fileList);
        gdk_clipboard_set_value(cb, &value);
        g_value_unset(&value);
    }

    void clear() override {
        GdkClipboard *cb = clipboard();
        if(cb == nullptr) return;
        // GTK 4 has no explicit clear; resetting content to none empties the
        // local clipboard offer.
        gdk_clipboard_set_content(cb, nullptr);
    }

private:
    struct TextReadState { bool done = false; OmegaCommon::String text; };
    struct FileReadState { bool done = false; OmegaCommon::Vector<OmegaCommon::FS::Path> paths; };

    /// Resolve the shared system clipboard, or null when no display is up.
    static GdkClipboard * clipboard() {
        GdkDisplay *display = gdk_display_get_default();
        if(display == nullptr) return nullptr;
        return gdk_display_get_clipboard(display);
    }

    /// Iterate the default main context until the async read completes. This is
    /// the documented sync-over-async path for selection-protocol backends.
    static void pumpUntil(const bool & done){
        while(!done){
            g_main_context_iteration(nullptr, TRUE); // block for the next event
        }
    }

    static void onTextRead(GObject *source, GAsyncResult *res, gpointer data){
        auto *state = static_cast<TextReadState *>(data);
        GError *error = nullptr;
        char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), res, &error);
        if(text != nullptr){
            state->text = OmegaCommon::String(text);
            g_free(text);
        }
        if(error != nullptr) g_error_free(error);
        state->done = true;
    }

    static void onFileRead(GObject *source, GAsyncResult *res, gpointer data){
        auto *state = static_cast<FileReadState *>(data);
        GError *error = nullptr;
        const GValue *value = gdk_clipboard_read_value_finish(GDK_CLIPBOARD(source), res, &error);
        if(value != nullptr && G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)){
            GdkFileList *fileList = static_cast<GdkFileList *>(g_value_get_boxed(value));
            if(fileList != nullptr){
                GSList *files = gdk_file_list_get_files(fileList); // transfer container
                for(GSList *node = files; node != nullptr; node = node->next){
                    GFile *file = G_FILE(node->data);
                    char *path = g_file_get_path(file);
                    if(path != nullptr){
                        state->paths.push_back(OmegaCommon::FS::Path(std::string(path)));
                        g_free(path);
                    }
                }
                g_slist_free(files);
            }
        }
        if(error != nullptr) g_error_free(error);
        state->done = true;
    }
};


}

namespace OmegaWTK::Native {
    NativeClipboardPtr get_native_clipboard(){
        static NativeClipboardPtr instance = std::make_shared<GTK::GTKClipboard>();
        return instance;
    }
}
