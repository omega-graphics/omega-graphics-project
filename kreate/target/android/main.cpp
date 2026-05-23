#include <kreate/App.h>

// Stub Android entry point. KREATE on Android is a shared library; a real
// NativeActivity / android_native_app_glue bootstrap that drives the app's
// run loop from ALooper events is a follow-up task. This symbol exists so
// the .so links cleanly and downstream integrators have a known anchor.

extern "C" __attribute__((visibility("default")))
int kreate_android_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    auto app = Kreate::CreateApp();
    app->run();
    return 0;
}
