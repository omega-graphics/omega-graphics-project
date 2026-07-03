#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Native/NativeTheme.h"
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
class AppWindow;
class ThemeVars;
class Theme;

class OMEGAWTK_EXPORT AppInst {
    Native::NAP ptr;
    static AppInst *instance;
    // The default.pak shipped beside the executable. Opened in the
    // AppInst constructor and required: missing or unreadable pak is
    // a hard error. Consumed today by PipelineRegistry::initialize() to
    // stream the compositor shader library; user code may also load
    // app-specific assets out of it via getAssetBundle().
    OmegaCommon::AssetBundle assetBundle;
    // Widget-View-Paint-Lifecycle-Plan Tier D / D7.1 (2026-06-04):
    // process-wide active theme. Referenced by sheet-rule `Var{name}`
    // values; the `StyleResolver` substitutes through this map at
    // cascade time. Null until app code calls `setThemeVars(...)` —
    // a null theme makes every `Var` resolve to "unbound" and the
    // resolver skips the cell (the inline-`Style` path still runs).
    SharedHandle<ThemeVars> themeVars_;
    // Native-Theme-Application-Plan Tier 1 (2026-06-30): the most
    // recently observed OS theme. Seeded from `Native::queryCurrentTheme()`
    // at construction and refreshed by the per-platform theme observer
    // (Cocoa KVO on `effectiveAppearance`, Win32 `WM_SETTINGCHANGE`/
    // `WM_THEMECHANGED`, GTK `notify::gtk-*-theme` on `GtkSettings`),
    // which routes through `onThemeSet(...)`. Tier 1 is observational
    // only — nothing reads this into the render clear yet; that is
    // Tier 2's `resolveWindowSurfaceColor`.
    Native::ThemeDesc nativeTheme_;
    // Native-Theme-Application-Plan Tier 3 (2026-07-01): the active
    // app-registered custom theme, or null to fall through to the OS
    // theme. When set, its variant `surface` overrides the clear color
    // (priority-chain row 2) and its variant `vars` back `themeVars()`.
    SharedHandle<Theme> theme_;
    // Tier 3: when set, the app pins the appearance (Light/Dark) instead
    // of following the OS bit. Drives both the custom-theme variant pick
    // and the row-4 clear fallback. Empty = follow `nativeTheme_.appearance`.
    Core::Optional<Native::ThemeAppearance> forcedAppearance_;
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
    /// Tier 3: mark every known window's cascade dirty after a theme
    /// input (`themeVars_`, `theme_`, `forcedAppearance_`) changes, so
    /// the next frame re-resolves cells and re-derives the surface color.
    void dirtyThemeCascade();
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
    OmegaCommon::AssetBundle & getAssetBundle();
    const OmegaCommon::AssetBundle & getAssetBundle() const;

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

    /// Native-Theme-Application-Plan Tier 1 (2026-06-30): the most
    /// recently observed OS theme (`ThemeAppearance` + `Colors` +
    /// `Typography`). Seeded at construction and updated on every OS
    /// appearance / theme change by the platform observer via
    /// `onThemeSet(...)`. Returns the cached value — no OS query is
    /// issued per call.
    const Native::ThemeDesc & nativeTheme() const;

    /// Native-Theme-Application-Plan Tier 1 (2026-06-30): observer
    /// trampoline. Invoked by the per-platform OS theme observer with
    /// a freshly-queried `ThemeDesc` when the system light/dark setting
    /// (or theme) changes. Caches the desc (see `nativeTheme()`) and
    /// fans the change out to the `AppWindowManager`, whose observer
    /// chain re-runs `onThemeSet` down every window's widget tree.
    void onThemeSet(Native::ThemeDesc & desc);

    /// Native-Theme-Application-Plan Tier 2 (2026-07-01): resolve the
    /// window "surface" color (the per-frame clear) per the plan's §3
    /// priority chain. This tier implements rows 3 (the cached OS theme
    /// `background`) and 4 (a hardcoded appearance fallback, reached only
    /// if the theme is somehow unset). Row 1 (an explicit background on
    /// the root `UIView`) is already realized by the root's
    /// background-rect paint, and row 2 (a custom `Theme` override) lands
    /// in Tier 3. Called by the FrameBuilder Style-phase hook, which
    /// writes the result into `AppWindow::setSurfaceColor`.
    Composition::Color resolveWindowSurfaceColor(const AppWindow & win) const;

    /// Native-Theme-Application-Plan Tier 3 (2026-07-01): install (or
    /// clear, with a null handle) the active custom `Theme`. While a
    /// theme is set, `resolveWindowSurfaceColor` returns the active
    /// variant's `surface` (priority-chain row 2) and `themeVars()`
    /// returns the active variant's `vars`. Swapping the theme dirties
    /// every known window's cascade so the next frame re-resolves against
    /// the new bindings — the same path `setThemeVars` uses.
    void setTheme(SharedHandle<Theme> theme);
    SharedHandle<Theme> theme() const;

    /// Tier 3: pin the appearance regardless of the OS setting. Pass a
    /// value to force Light or Dark (the custom theme, if any, still
    /// picks its matching variant); pass an empty optional to resume
    /// following the OS. Dirties every window's cascade like `setTheme`.
    void setForcedAppearance(Core::Optional<Native::ThemeAppearance> appearance);
    Core::Optional<Native::ThemeAppearance> forcedAppearance() const;

    /// Tier 3: the appearance in effect — `forcedAppearance()` if set,
    /// else `nativeTheme().appearance`. This is the bit that selects a
    /// custom theme's variant and the row-4 clear fallback.
    Native::ThemeAppearance activeAppearance() const;

    ~AppInst();
};


    
};

// #ifdef TARGET_WIN32 
// #ifdef WINDOWS_PRIVATE

// OMEGAWTK_EXPORT RECT get_hwnd_item_coords(void * hwnd);

// #endif
// #endif

#endif
