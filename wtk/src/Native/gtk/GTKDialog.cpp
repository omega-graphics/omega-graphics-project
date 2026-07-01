#include "omegaWTK/Native/NativeDialog.h"

// X11's <X.h> (pulled in transitively via GTK) defines the macro
// `CursorShape` which collides with OmegaWTK::Native::CursorShape.
#ifdef CursorShape
#undef CursorShape
#endif

#include "GTKApp.h"

#include <gtk/gtk.h>
#include <cctype>
#include <memory>

namespace OmegaWTK::Native::GTK {


// GTK 4: gtk_dialog_run and the GtkDialog widgets are gone. File and alert
// dialogs are the async GtkFileDialog / GtkAlertDialog (4.10+), which fit the
// NativeFSDialog/NativeAlertDialog Async<> contract directly — the GAsyncResult
// callback resolves the promise. Following the Cocoa backend, the promise is a
// shared_ptr carried into the callback (via a heap context) so it outlives the
// dialog object. The bare-surface backend has no GtkWindow, so dialogs are
// parented to NULL (non-transient) for now.

    static OmegaCommon::String toLowerAscii(const OmegaCommon::String & s){
        OmegaCommon::String l;
        for(char c : s) l.push_back((char)std::tolower((unsigned char)c));
        return l;
    }

    class GTKFSDialog : public NativeFSDialog {
        enum class Mode { OpenSingle, OpenMultiple, Save };
        bool read;
        bool allowMultiple;
        OmegaCommon::String openLocation;
        OmegaCommon::Vector<FileFilter> filters;
        std::shared_ptr<OmegaCommon::Promise<OmegaCommon::Vector<OmegaCommon::FS::Path>>> promise;

        struct CallbackCtx {
            std::shared_ptr<OmegaCommon::Promise<OmegaCommon::Vector<OmegaCommon::FS::Path>>> promise;
            Mode mode;
        };

        static void appendPath(OmegaCommon::Vector<OmegaCommon::FS::Path> & out, GFile *file){
            if(file == nullptr) return;
            char *path = g_file_get_path(file);
            if(path != nullptr){
                out.push_back(OmegaCommon::FS::Path(std::string(path)));
                g_free(path);
            }
        }

        static void onFinished(GObject *source, GAsyncResult *res, gpointer data){
            std::unique_ptr<CallbackCtx> ctx(static_cast<CallbackCtx *>(data));
            GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
            OmegaCommon::Vector<OmegaCommon::FS::Path> out;
            GError *error = nullptr;
            if(ctx->mode == Mode::OpenMultiple){
                GListModel *files = gtk_file_dialog_open_multiple_finish(dialog, res, &error);
                if(files != nullptr){
                    guint n = g_list_model_get_n_items(files);
                    for(guint i = 0; i < n; ++i){
                        GFile *file = G_FILE(g_list_model_get_item(files, i));
                        appendPath(out, file);
                        if(file) g_object_unref(file);
                    }
                    g_object_unref(files);
                }
            } else {
                GFile *file = (ctx->mode == Mode::Save)
                    ? gtk_file_dialog_save_finish(dialog, res, &error)
                    : gtk_file_dialog_open_finish(dialog, res, &error);
                appendPath(out, file);
                if(file) g_object_unref(file);
            }
            // A cancelled dialog reports G_IO_ERROR_CANCELLED; out stays empty,
            // matching the "empty when cancelled" contract.
            if(error) g_error_free(error);
            ctx->promise->set(out);
        }
    public:
        GTKFSDialog(const Descriptor & desc,NWH nativeWindow):
            NativeFSDialog(nativeWindow),
            read(desc.type == Read),
            allowMultiple(desc.allowMultiple),
            filters(desc.filters),
            promise(std::make_shared<OmegaCommon::Promise<OmegaCommon::Vector<OmegaCommon::FS::Path>>>()){
            openLocation = OmegaCommon::FS::Path(desc.openLocation).str();
        }

        OmegaCommon::Async<OmegaCommon::Vector<OmegaCommon::FS::Path>> getResult() override {
            GtkWindow *parent = gtk_window_from_native(parentWindow); // null on bare surface
            GtkFileDialog *dialog = gtk_file_dialog_new();
            gtk_file_dialog_set_title(dialog, read ? "Open" : "Save");

            if(!openLocation.empty()){
                GFile *folder = g_file_new_for_path(openLocation.c_str());
                gtk_file_dialog_set_initial_folder(dialog, folder);
                g_object_unref(folder);
            }

            if(!filters.empty()){
                GListStore *store = g_list_store_new(GTK_TYPE_FILE_FILTER);
                for(auto & f : filters){
                    GtkFileFilter *filter = gtk_file_filter_new();
                    gtk_file_filter_set_name(filter, f.label.c_str());
                    if(f.extensions.empty()){
                        gtk_file_filter_add_pattern(filter, "*");
                    } else {
                        for(auto & ext : f.extensions){
                            OmegaCommon::String pattern = OmegaCommon::String("*.") + ext;
                            gtk_file_filter_add_pattern(filter, pattern.c_str());
                        }
                    }
                    g_list_store_append(store, filter);
                    g_object_unref(filter);
                }
                gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(store));
                g_object_unref(store);
            }

            Mode mode = read ? (allowMultiple ? Mode::OpenMultiple : Mode::OpenSingle) : Mode::Save;
            auto *ctx = new CallbackCtx{promise, mode};
            switch(mode){
                case Mode::OpenMultiple:
                    gtk_file_dialog_open_multiple(dialog, parent, nullptr, onFinished, ctx);
                    break;
                case Mode::OpenSingle:
                    gtk_file_dialog_open(dialog, parent, nullptr, onFinished, ctx);
                    break;
                case Mode::Save:
                    gtk_file_dialog_save(dialog, parent, nullptr, onFinished, ctx);
                    break;
            }
            g_object_unref(dialog); // the async op holds its own ref to completion
            return promise->async();
        }
    };

    class GTKAlertDialog : public NativeAlertDialog {
        Descriptor desc;
        std::shared_ptr<OmegaCommon::Promise<Result>> promise;

        struct CallbackCtx {
            std::shared_ptr<OmegaCommon::Promise<Result>> promise;
            OmegaCommon::Vector<OmegaCommon::String> buttonLabels;
        };

        static Result resultForLabel(const OmegaCommon::String & label, bool isFirst){
            OmegaCommon::String l = toLowerAscii(label);
            if(l == "ok")     return Result::OK;
            if(l == "cancel") return Result::Cancel;
            if(l == "yes")    return Result::Yes;
            if(l == "no")     return Result::No;
            return isFirst ? Result::OK : Result::Cancel;
        }

        static void onChosen(GObject *source, GAsyncResult *res, gpointer data){
            std::unique_ptr<CallbackCtx> ctx(static_cast<CallbackCtx *>(data));
            GError *error = nullptr;
            int index = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, &error);
            Result r = Result::Cancel;
            if(error != nullptr){
                // Dismissed without a button (Escape / close) → Cancel.
                g_error_free(error);
            } else if(ctx->buttonLabels.empty()){
                r = (index == 0) ? Result::OK : Result::Cancel;
            } else if(index >= 0 && index < (int)ctx->buttonLabels.size()){
                r = resultForLabel(ctx->buttonLabels[(size_t)index], index == 0);
            }
            ctx->promise->set(r);
        }
    public:
        GTKAlertDialog(const Descriptor & desc,NWH nativeWindow):
            NativeAlertDialog(nativeWindow),
            desc(desc),
            promise(std::make_shared<OmegaCommon::Promise<Result>>()){}

        OmegaCommon::Async<Result> getResult() override {
            GtkWindow *parent = gtk_window_from_native(parentWindow); // null on bare surface
            GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", desc.title.c_str());
            if(!desc.message.empty()){
                gtk_alert_dialog_set_detail(dialog, desc.message.c_str());
            }
            gtk_alert_dialog_set_modal(dialog, TRUE);

            OmegaCommon::Vector<OmegaCommon::String> labels = desc.buttonLabels;
            if(labels.empty()) labels.push_back("OK");
            OmegaCommon::Vector<const char *> cbuttons;
            for(auto & l : labels) cbuttons.push_back(l.c_str());
            cbuttons.push_back(nullptr);
            gtk_alert_dialog_set_buttons(dialog, cbuttons.data());

            auto *ctx = new CallbackCtx{promise, desc.buttonLabels};
            gtk_alert_dialog_choose(dialog, parent, nullptr, onChosen, ctx);
            g_object_unref(dialog);
            return promise->async();
        }
    };

    class GTKNoteDialog : public NativeNoteDialog {
    public:
        GTKNoteDialog(const Descriptor & desc,NWH nativeWindow):NativeNoteDialog(nativeWindow){
            GtkWindow *parent = gtk_window_from_native(nativeWindow); // null on bare surface
            GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", desc.title.c_str());
            if(!desc.str.empty()){
                gtk_alert_dialog_set_detail(dialog, desc.str.c_str());
            }
            // Fire-and-forget informational dialog (no result to report).
            gtk_alert_dialog_show(dialog, parent);
            g_object_unref(dialog);
        }
    };


}

namespace OmegaWTK::Native {

    SharedHandle<NativeFSDialog> NativeFSDialog::Create(const Descriptor &desc, NWH nativeWindow){
        return SharedHandle<NativeFSDialog>(new GTK::GTKFSDialog(desc,nativeWindow));
    }

    SharedHandle<NativeAlertDialog> NativeAlertDialog::Create(const Descriptor &desc, NWH nativeWindow){
        return SharedHandle<NativeAlertDialog>(new GTK::GTKAlertDialog(desc,nativeWindow));
    }

    SharedHandle<NativeNoteDialog> NativeNoteDialog::Create(const Descriptor &desc, NWH nativeWindow){
        return SharedHandle<NativeNoteDialog>(new GTK::GTKNoteDialog(desc,nativeWindow));
    }

}
