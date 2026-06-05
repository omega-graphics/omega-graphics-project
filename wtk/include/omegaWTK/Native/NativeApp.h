#include "omegaWTK/Core/Core.h"
#include "omega-common/fs.h"

#ifndef OMEGAWTK_NATIVE_NATIVEAPP_H
#define OMEGAWTK_NATIVE_NATIVEAPP_H

namespace OmegaWTK::Native {

struct NativeAppLaunchArgs {
    int argc = 0;
    char **argv = nullptr;
};

/// App-lifecycle hooks the host can install on `NativeApp`. Every method
/// has a no-op default so partial implementations are legal.
///
/// `onOpenFile` / `onOpenURL` are routed from the OS file-association /
/// URL-scheme dispatch machinery — macOS `NSApplicationDelegate`
/// `application:openFile:` / `application:openURLs:`, GTK
/// `GApplication::open` (the `GtkApplication` is created with
/// `G_APPLICATION_HANDLES_OPEN`). Win32 has no equivalent intra-process
/// hook in v0; see the §2.4 implementation note in
/// `wtk/.plans/Native-API-Completion-Proposal.md`.
///
/// All callbacks fire on the UI / main thread.
class OMEGAWTK_EXPORT NativeAppDelegate {
public:
    virtual void onAppReady() {}
    virtual void onAppWillTerminate() {}
    virtual void onOpenFile(const OmegaCommon::FS::Path & path) { (void)path; }
    virtual void onOpenURL(const OmegaCommon::String & url) { (void)url; }
    virtual ~NativeAppDelegate() = default;
};

class OMEGAWTK_EXPORT NativeApp {
public:
    virtual int runEventLoop() = 0;
    virtual void terminate() = 0;

    /// Install (or clear, with `nullptr`) the lifecycle delegate.
    /// Non-owning — caller keeps ownership of `delegate`. Replacing a
    /// previously-installed delegate is allowed; backends do not call
    /// into the prior delegate after this returns.
    void setDelegate(NativeAppDelegate * delegate);

    /// The `NativeAppLaunchArgs` adopted at construction time. Backends
    /// that received `nullptr` for `data` (e.g. Win32 entry points that
    /// don't surface argc/argv) report `argc = 0`, `argv = nullptr`
    /// here — use `commandLineArgs()` instead, which falls back to the
    /// platform's own command-line API.
    const NativeAppLaunchArgs & launchArgs() const;

    /// UTF-8 command-line argument vector. Pulls from `launchArgs()` when
    /// available; otherwise the platform backend falls back to its native
    /// command-line API (`CommandLineToArgvW` on Win32). The first entry
    /// is the program name when the platform supplies it.
    virtual OmegaCommon::Vector<OmegaCommon::String> commandLineArgs() const;

    NativeApp();
    virtual ~NativeApp() = default;

protected:
    /// Backends call this from their own ctor when `make_native_app`'s
    /// `data` argument is a `NativeAppLaunchArgs *`. Copies the pointer
    /// fields by value (the underlying argv array is owned by the
    /// caller, i.e. the entry-point shim — its lifetime exceeds
    /// `NativeApp`'s).
    void adoptLaunchArgs(const NativeAppLaunchArgs & args);

    NativeAppDelegate * delegate_ = nullptr;
    NativeAppLaunchArgs launchArgs_;
};
typedef SharedHandle<NativeApp> NAP;

/// Construct the platform `NativeApp`.
///
/// The `data` pointer's meaning is **per-platform**, dictated by the
/// matching per-target entry-point shim under `wtk/target/`:
///
/// - **macOS** (`target/macos/main.mm`) — `data` is `nullptr`. Cocoa
///   reads `argc`/`argv` via `[NSProcessInfo processInfo]` anyway.
/// - **Win32** (`target/win32/mmain.cpp`) — `data` is the
///   `HINSTANCE` passed to `WinMain`, cast to `void *`. The Win32
///   backend forwards it to `HWNDFactory(HINSTANCE)`. It is NOT a
///   `NativeAppLaunchArgs *`. Win32 `commandLineArgs()` falls back
///   to `CommandLineToArgvW(GetCommandLineW())`.
/// - **Linux/GTK** (`target/gtk/main.cpp`) and the generic
///   `target/AppEntryPoint.cpp` shim — `data` is a
///   `NativeAppLaunchArgs *` owned by the caller. The GTK backend
///   adopts it via `adoptLaunchArgs(...)`.
///
/// Backends that need `argc`/`argv` and may receive a non-args
/// `data` (Win32) override `commandLineArgs()` to surface the OS's
/// own command-line API.
NAP make_native_app(void *data);
//void free_native_app();

// NAMI make_menu_item(Core::String & str,bool hasSubMenu,bool isSeperator = false,NAM subMenuPtr = nullptr);
// NAM make_menu();

};

#ifdef TARGET_WIN32

#include <Windows.h>

void * __create_hwnd_factory(void *hinst);
void * __hwnd_factory_get_all_hwnds(void * hwnd_factory);
void __free_hwnd_factory(void *hwnd_factory);
RECT __get_hwnd_real_coords(HWND hwnd);
#endif


// #ifdef TARGET_MACOS
// int cocoa_app_init(int argc,char * argv[]);
// #endif};

#endif
