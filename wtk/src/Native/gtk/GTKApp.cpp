#include "omegaWTK/Native/NativeApp.h"
#include "GTKApp.h"

#include <gtk/gtk.h>
#include <iostream>
#ifdef WTK_NATIVE_X11
#include <X11/Xlib.h>
#endif

namespace OmegaWTK::Native::GTK {
    namespace {
        GtkApplication *gActiveApp = nullptr;

        void on_app_activate(GtkApplication *app,gpointer userData){
            (void)app;
            (void)userData;
        }
    }

    GtkApplication *get_active_application() {
        return gActiveApp;
    }

    class GTKApp : public NativeApp {
        GtkApplication *native = nullptr;
        int argc = 0;
        char **argv = nullptr;
    public:
        void terminate() override{
            if(native != nullptr){
                g_application_quit(G_APPLICATION(native));
            }
        };
        explicit GTKApp(void *data){
#ifdef WTK_NATIVE_X11
            static bool x11ThreadsInitAttempted = false;
            if(!x11ThreadsInitAttempted){
                x11ThreadsInitAttempted = true;
                if(XInitThreads() == 0){
                    std::cerr << "[OmegaWTK][GTK] XInitThreads failed; X11 access from multiple threads may be unsafe." << std::endl;
                }
            }
#endif
            native = gtk_application_new("org.omegagraphics.wtk",G_APPLICATION_FLAGS_NONE);
            if(data != nullptr){
                auto *launchArgs = static_cast<NativeAppLaunchArgs *>(data);
                argc = launchArgs->argc;
                argv = launchArgs->argv;
            }
            if(native != nullptr){
                g_signal_connect(native,"activate",G_CALLBACK(on_app_activate),nullptr);
            }
            if(native != nullptr && !g_application_get_is_registered(G_APPLICATION(native))){
                GError *registerError = nullptr;
                if(!g_application_register(G_APPLICATION(native),nullptr,&registerError)){
                    std::cerr << "[OmegaWTK][GTK] Failed to register GtkApplication";
                    if(registerError != nullptr){
                        std::cerr << ": " << registerError->message;
                        g_error_free(registerError);
                    }
                    std::cerr << std::endl;
                }
            }
            gActiveApp = native;
//            g_signal_connect(native,"activate",NULL,NULL);
        };
        int runEventLoop() override {
            return g_application_run(G_APPLICATION(native),argc,argv);
        }
        ~GTKApp() override {
            if(gActiveApp == native){
                gActiveApp = nullptr;
            }
            if(native != nullptr){
                g_object_unref(native);
                native = nullptr;
            }
        }
    };

}

namespace OmegaWTK::Native {
    NAP make_native_app(void *data){
        return (NAP)new GTK::GTKApp(data);
    };
}
