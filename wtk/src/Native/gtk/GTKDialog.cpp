#include "omegaWTK/Native/NativeDialog.h"

// X11's <X.h> (pulled in transitively via GTK) defines the macro
// `CursorShape` which collides with OmegaWTK::Native::CursorShape.
#ifdef CursorShape
#undef CursorShape
#endif

#include "GTKApp.h"

#include <gtk/gtk.h>
#include <cctype>

namespace OmegaWTK::Native::GTK {

    class GTKFSDialog : public NativeFSDialog {
        bool read;
        bool allowMultiple;
        OmegaCommon::String openLocation;
        OmegaCommon::Vector<FileFilter> filters;
        OmegaCommon::Promise<OmegaCommon::Vector<OmegaCommon::FS::Path>> promise;
    public:
        GTKFSDialog(const Descriptor & desc,NWH nativeWindow):
            NativeFSDialog(nativeWindow),
            read(desc.type == Read),
            allowMultiple(desc.allowMultiple),
            filters(desc.filters){
            OmegaCommon::FS::Path loc = desc.openLocation;
            openLocation = loc.str();
        }

        OmegaCommon::Async<OmegaCommon::Vector<OmegaCommon::FS::Path>> getResult() override {
            OmegaCommon::Vector<OmegaCommon::FS::Path> out;
            GtkWindow *parent = gtk_window_from_native(parentWindow);

            GtkFileChooserAction action = read ? GTK_FILE_CHOOSER_ACTION_OPEN
                                                : GTK_FILE_CHOOSER_ACTION_SAVE;
            const gchar *acceptLabel = read ? "_Open" : "_Save";

            GtkWidget *dialog = gtk_file_chooser_dialog_new(
                read ? "Open" : "Save",
                parent,
                action,
                "_Cancel", GTK_RESPONSE_CANCEL,
                acceptLabel, GTK_RESPONSE_ACCEPT,
                nullptr);
            GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);

            if(read)
                gtk_file_chooser_set_select_multiple(chooser, allowMultiple ? TRUE : FALSE);

            if(!openLocation.empty())
                gtk_file_chooser_set_current_folder(chooser, openLocation.c_str());

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
                gtk_file_chooser_add_filter(chooser, filter);
            }

            if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT){
                if(read && allowMultiple){
                    GSList *list = gtk_file_chooser_get_filenames(chooser);
                    for(GSList *node = list; node != nullptr; node = node->next){
                        gchar *name = (gchar *)node->data;
                        out.push_back(OmegaCommon::FS::Path(name));
                        g_free(name);
                    }
                    g_slist_free(list);
                } else {
                    gchar *name = gtk_file_chooser_get_filename(chooser);
                    if(name){
                        out.push_back(OmegaCommon::FS::Path(name));
                        g_free(name);
                    }
                }
            }

            gtk_widget_destroy(dialog);
            promise.set(out);
            return promise.async();
        }
    };

    class GTKAlertDialog : public NativeAlertDialog {
        Descriptor desc;
        OmegaCommon::Promise<Result> promise;

        static Result resultForLabel(const OmegaCommon::String & label, bool isFirst){
            OmegaCommon::String l;
            for(char c : label) l.push_back((char)std::tolower((unsigned char)c));
            if(l == "ok")     return Result::OK;
            if(l == "cancel") return Result::Cancel;
            if(l == "yes")    return Result::Yes;
            if(l == "no")      return Result::No;
            return isFirst ? Result::OK : Result::Cancel;
        }
    public:
        GTKAlertDialog(const Descriptor & desc,NWH nativeWindow):
            NativeAlertDialog(nativeWindow),desc(desc){}

        OmegaCommon::Async<Result> getResult() override {
            GtkWindow *parent = gtk_window_from_native(parentWindow);

            GtkMessageType type = GTK_MESSAGE_INFO;
            switch(desc.style){
                case Style::Info:    type = GTK_MESSAGE_INFO; break;
                case Style::Warning: type = GTK_MESSAGE_WARNING; break;
                case Style::Error:   type = GTK_MESSAGE_ERROR; break;
            }

            GtkWidget *dialog = gtk_message_dialog_new(
                parent,
                GTK_DIALOG_MODAL,
                type,
                GTK_BUTTONS_NONE,
                "%s", desc.title.c_str());
            if(!desc.message.empty()){
                gtk_message_dialog_format_secondary_text(
                    GTK_MESSAGE_DIALOG(dialog), "%s", desc.message.c_str());
            }

            // Response ids index into buttonLabels; the empty-labels case adds
            // a single OK button at index 0.
            if(desc.buttonLabels.empty()){
                gtk_dialog_add_button(GTK_DIALOG(dialog), "OK", 0);
            } else {
                for(size_t i = 0; i < desc.buttonLabels.size(); ++i)
                    gtk_dialog_add_button(GTK_DIALOG(dialog), desc.buttonLabels[i].c_str(), (gint)i);
            }

            gint response = gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);

            Result res = Result::Cancel;
            if(desc.buttonLabels.empty()){
                res = (response == 0) ? Result::OK : Result::Cancel;
            } else if(response >= 0 && response < (gint)desc.buttonLabels.size()){
                res = resultForLabel(desc.buttonLabels[response], response == 0);
            }
            promise.set(res);
            return promise.async();
        }
    };

    class GTKNoteDialog : public NativeNoteDialog {
    public:
        GTKNoteDialog(const Descriptor & desc,NWH nativeWindow):NativeNoteDialog(nativeWindow){
            GtkWindow *parent = gtk_window_from_native(nativeWindow);
            GtkWidget *dialog = gtk_message_dialog_new(
                parent,
                GTK_DIALOG_MODAL,
                GTK_MESSAGE_INFO,
                GTK_BUTTONS_OK,
                "%s", desc.title.c_str());
            if(!desc.str.empty()){
                gtk_message_dialog_format_secondary_text(
                    GTK_MESSAGE_DIALOG(dialog), "%s", desc.str.c_str());
            }
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
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
