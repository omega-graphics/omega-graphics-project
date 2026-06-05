#include "omegaWTK/Core/Core.h"
#include "omega-common/assets.h"
#include "omega-common/fs.h"

#ifndef OMEGAWTK_UI_APP_H
#define OMEGAWTK_UI_APP_H


namespace OmegaWTK {
    namespace Native {
        class NativeApp;
        typedef SharedHandle<NativeApp> NAP;
    };

class AppWindowManager;
class ThemeVars;

class OMEGAWTK_EXPORT AppInst {
    Native::NAP ptr;
    static AppInst *instance;
    OmegaCommon::Optional<OmegaCommon::AssetBundle> assetBundle;
    // Widget-View-Paint-Lifecycle-Plan Tier D / D7.1 (2026-06-04):
    // process-wide active theme. Referenced by sheet-rule `Var{name}`
    // values; the `StyleResolver` substitutes through this map at
    // cascade time. Null until app code calls `setThemeVars(...)` —
    // a null theme makes every `Var` resolve to "unbound" and the
    // resolver skips the cell (the inline-`Style` path still runs).
    SharedHandle<ThemeVars> themeVars_;
    // Guards against a future caller invoking doShutdown() more than
    // once (the destructor is currently the sole call site, but the
    // idempotency keeps the contract safe to expand).
    bool shutdownDone_ = false;
    /// Synchronous teardown of windowManager, FontEngine, Composition,
    /// and GTE. Called only from ~AppInst so that every user-held
    /// AppWindow SharedHandle has already dropped — the windowManager
    /// is the last owner and dropping it releases each AppWindow's
    /// backend resources back to the still-live GTE device. Doing the
    /// teardown earlier (inside start() right after runEventLoop
    /// returns) closes the device under any still-live caller-held
    /// window and surfaces as D3D12MA errors on Windows.
    void doShutdown();
public:
    OMEGACOMMON_CLASS("OmegaWTK.AppInst")


    static  AppInst * inst();
    UniqueHandle<AppWindowManager> windowManager;

    explicit AppInst(void *data);

    static int start();

    static void terminate();

    /**
     @brief Absolute path of the directory containing the running
            executable.

     Resolved via the platform "module path" API
     (`GetModuleFileNameA` on Windows, `_NSGetExecutablePath` on macOS,
     `readlink("/proc/self/exe")` on Linux). CWD-independent — works
     from a debugger, ctest, double-click, etc. — so tests and apps
     can locate assets staged next to the binary regardless of how
     they were launched.

     Returns an empty Path if the platform call fails (a real failure,
     never normal-launch behaviour).

     @returns FS::Path
    */
    static OmegaCommon::FS::Path executableDir();


// #ifdef TARGET_WIN32
//     AppInst(void * windows_inst);
// #endif
    Native::NAP & getNAP();
    OmegaCommon::AssetBundle * getAssetBundle();
    const OmegaCommon::AssetBundle * getAssetBundle() const;

    /// Widget-View-Paint-Lifecycle-Plan Tier D / D7.1 (2026-06-04):
    /// the active process-wide theme. Returns the currently installed
    /// `ThemeVars` handle, or a null handle if app code has not called
    /// `setThemeVars(...)`. The `StyleResolver` uses this to substitute
    /// `StyleSheets::Var{name}` rule values during the Style phase.
    SharedHandle<ThemeVars> themeVars() const;
    /// Swap the active theme. The replacement handle takes effect on
    /// the next frame: every known `AppWindow` is marked
    /// style-dirty so the resolver re-walks every cascade against the
    /// new bindings. Passing a null handle clears the active theme —
    /// subsequent `Var{name}` lookups resolve as unbound and the
    /// resolver skips those cells, matching CSS `var()` fallthrough.
    void setThemeVars(SharedHandle<ThemeVars> theme);

    ~AppInst();
};


    
};

// #ifdef TARGET_WIN32 
// #ifdef WINDOWS_PRIVATE

// OMEGAWTK_EXPORT RECT get_hwnd_item_coords(void * hwnd);

// #endif
// #endif

#endif
