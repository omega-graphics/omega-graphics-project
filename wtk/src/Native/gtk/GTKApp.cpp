#include "omegaWTK/Native/NativeApp.h"
#include "omegaWTK/Native/NativeTheme.h"
#include "omegaWTK/UI/App.h"
#include "GTKApp.h"

#include <gtk/gtk.h>
#include <iostream>
#ifdef WTK_NATIVE_X11
#include <X11/Xlib.h>
#endif

namespace OmegaWTK::Native::GTK {
    namespace {
        GtkApplication *gActiveApp = nullptr;

        void on_app_activate(GtkApplication *app,gpointer userData);
        void on_app_shutdown(GApplication *app,gpointer userData);
        void on_app_open(GApplication *app,GFile **files,gint nFiles,
                          const gchar *hint,gpointer userData);
        void on_theme_notify(GObject *settings,GParamSpec *pspec,gpointer userData);
    }

    GtkApplication *get_active_application() {
        return gActiveApp;
    }

    class GTKApp : public NativeApp {
        GtkApplication *native = nullptr;
        int argc = 0;
        char **argv = nullptr;
        // GtkApplication auto-decrements its use count on every window
        // close (gtk_application_remove_window); reaching zero quits the
        // GApplication and ends g_application_run. We promise the
        // public API that only `AppInst::terminate()` shuts the app
        // down — closing a window must leave the process running so
        // the host can open another, sit headless, or wait on a
        // background task. A persistent `g_application_hold` keeps the
        // use count ≥ 1 regardless of window count; `terminate()`
        // drops it before quitting so GApplication's accounting stays
        // balanced.
        bool holdActive = false;
    public:
        void terminate() override{
            // Delegate notification is routed through the GApplication
            // `shutdown` signal (see `on_app_shutdown` below) so that
            // explicit terminate, SIGTERM, OS session-end, and any
            // other path that ends `g_application_run` all surface as a
            // single `delegate_->onAppWillTerminate()` invocation.
            // Firing it here too would double-call the delegate on the
            // explicit-terminate path while leaving the external-shutdown
            // path with the single signal-driven call — keep them
            // symmetric by going through the signal in both cases.
            if(native != nullptr){
                if(holdActive){
                    g_application_release(G_APPLICATION(native));
                    holdActive = false;
                }
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
            // G_APPLICATION_HANDLES_OPEN enables the `open` signal so the
            // OS / file manager can hand us file paths and URIs after
            // launch; we route those to NativeAppDelegate::onOpenFile /
            // onOpenURL.
            native = gtk_application_new("org.omegagraphics.wtk",G_APPLICATION_HANDLES_OPEN);
            if(data != nullptr){
                auto *launchArgs = static_cast<NativeAppLaunchArgs *>(data);
                argc = launchArgs->argc;
                argv = launchArgs->argv;
                adoptLaunchArgs(*launchArgs);
            }
            if(native != nullptr){
                g_signal_connect(native,"activate",G_CALLBACK(on_app_activate),this);
                g_signal_connect(native,"shutdown",G_CALLBACK(on_app_shutdown),this);
                g_signal_connect(native,"open",G_CALLBACK(on_app_open),this);
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
            // Bump the GApplication use count so the last window
            // closing does not auto-quit (see `holdActive` rationale
            // on the field). Safe to call after register; held until
            // `terminate()` releases.
            if(native != nullptr){
                g_application_hold(G_APPLICATION(native));
                holdActive = true;
            }
            gActiveApp = native;
        };
        int runEventLoop() override {
            return g_application_run(G_APPLICATION(native),argc,argv);
        }
        /// Accessor for the GTK signal-handler trampolines to invoke
        /// the installed app-level delegate without exposing
        /// `delegate_` outside the class.
        NativeAppDelegate * appLevelDelegate() const { return delegate_; }
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

    namespace {
        void on_app_activate(GtkApplication *app,gpointer userData){
            (void)app;
            auto *self = static_cast<GTKApp *>(userData);
            if(self == nullptr) return;
            // Native-Theme-Application-Plan Tier 1 (2026-06-30): connect
            // the OS theme observer here rather than in the GTKApp ctor.
            // GtkSettings has no default until GTK is initialised and a
            // display is open, which happens during g_application_run's
            // startup — by `activate` it exists. Connect once (GtkSettings
            // is a per-display singleton, so a second activate would
            // otherwise stack duplicate handlers).
            static bool themeObserverConnected = false;
            if(!themeObserverConnected){
                if(GtkSettings *settings = gtk_settings_get_default(); settings != nullptr){
                    g_signal_connect(settings,"notify::gtk-application-prefer-dark-theme",
                                     G_CALLBACK(on_theme_notify),nullptr);
                    g_signal_connect(settings,"notify::gtk-theme-name",
                                     G_CALLBACK(on_theme_notify),nullptr);
                    themeObserverConnected = true;
                }
            }
            if(auto *delegate = self->appLevelDelegate(); delegate != nullptr){
                delegate->onAppReady();
            }
        }

        void on_theme_notify(GObject *settings,GParamSpec *pspec,gpointer userData){
            // Native-Theme-Application-Plan Tier 1 (2026-06-30): the OS
            // dark/light preference or theme name changed. Re-query the
            // theme and hand it to the AppInst trampoline, which caches
            // it and fans the change out to every window's widget tree.
            (void)settings;
            (void)pspec;
            (void)userData;
            OmegaWTK::Native::ThemeDesc desc = OmegaWTK::Native::queryCurrentTheme();
            if(auto *app = OmegaWTK::AppInst::inst(); app != nullptr){
                app->onThemeSet(desc);
            }
        }

        void on_app_shutdown(GApplication *app,gpointer userData){
            (void)app;
            auto *self = static_cast<GTKApp *>(userData);
            if(self == nullptr) return;
            if(auto *delegate = self->appLevelDelegate(); delegate != nullptr){
                delegate->onAppWillTerminate();
            }
        }

        void on_app_open(GApplication *app,GFile **files,gint nFiles,
                         const gchar *hint,gpointer userData){
            (void)app;
            (void)hint;
            auto *self = static_cast<GTKApp *>(userData);
            if(self == nullptr) return;
            auto *delegate = self->appLevelDelegate();
            // The `open` signal is the first signal in the activation
            // sequence when files are passed; if no delegate is
            // installed we still fire `activate` ourselves so the app
            // gets a chance to bring up its UI.
            if(delegate == nullptr){
                g_application_activate(app);
                return;
            }
            for(gint i = 0; i < nFiles; ++i){
                GFile *file = files[i];
                if(file == nullptr) continue;
                gchar *scheme = g_file_get_uri_scheme(file);
                if(scheme != nullptr && g_strcmp0(scheme,"file") == 0){
                    gchar *path = g_file_get_path(file);
                    if(path != nullptr){
                        delegate->onOpenFile(OmegaCommon::FS::Path(std::string(path)));
                        g_free(path);
                    }
                } else {
                    gchar *uri = g_file_get_uri(file);
                    if(uri != nullptr){
                        delegate->onOpenURL(OmegaCommon::String(uri));
                        g_free(uri);
                    }
                }
                if(scheme != nullptr){
                    g_free(scheme);
                }
            }
            // GtkApplication does not auto-emit `activate` when `open`
            // handles the dispatch; do it so the app's normal startup
            // path still runs after files are processed.
            g_application_activate(app);
        }
    }

}

namespace OmegaWTK::Native {
    NAP make_native_app(void *data){
        return (NAP)new GTK::GTKApp(data);
    };
}
