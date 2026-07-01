#include "omegaWTK/UI/App.h"
#include "omegaWTK/Composition/FontEngine.h"
#include "omegaWTK/Core/GTEHandle.h"

#include "omegaWTK/Composition/CompositorClient.h"
#include "../Composition/Compositor.h"
#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/UI/ThemeVars.h"

#include "omega-common/assets.h"
#include "omega-common/unicode.h"

#include "omegaWTK/Native/NativeApp.h"
#include "omegaWTK/Native/NativeTheme.h"

#include <stdexcept>
#include <string>

#if defined(_WIN32)
  #include <windows.h>
#elif defined(__APPLE__)
  #include <mach-o/dyld.h>
  #include <climits>
#else
  #include <unistd.h>
  #include <climits>
#endif

namespace OmegaWTK {



AppInst * AppInst::instance;

AppInst *AppInst::inst() {
    return instance;
}

AppInst::AppInst(void *data):ptr(Native::make_native_app(data)),windowManager(std::make_unique<AppWindowManager>()){
    instance = this;
    // Native-Theme-Application-Plan Tier 1 (2026-06-30): seed the cached
    // OS theme now that the native app exists (Cocoa needs a live NSApp;
    // GTK returns ThemeDesc defaults until a display opens, then the
    // observer refreshes it). The platform observer keeps this current
    // on every subsequent light/dark flip via onThemeSet(...).
    nativeTheme_ = Native::queryCurrentTheme();
    gte = OmegaGTE::InitWithDefaultDevice();

    // Open the app's default.pak from beside the executable (Resources/
    // on macOS, exe-dir on Win/Linux — OmegaWTKApp.cmake stages it
    // either way). Mandatory: the compositor shader library is an
    // entry inside it and Composition::InitializeEngine() below relies
    // on it. CWD is unreliable (debugger / ctest / Finder launches all
    // leave it elsewhere than the binary), so resolve from the exe.
    OmegaCommon::FS::Path bundlePath = AppInst::executableDir();
#if defined(TARGET_MACOS)
    // .../Contents/MacOS -> .../Contents/Resources/default.pak
    {
        auto &exeDir = bundlePath.str();
        auto parentSlash = exeDir.rfind('/');
        if(parentSlash != std::string::npos){
            bundlePath = OmegaCommon::FS::Path(exeDir.substr(0, parentSlash) + "/Resources/default.pak");
        } else {
            bundlePath.append("default.pak");
        }
    }
#else
    bundlePath.append("default.pak");
#endif
    auto bundleResult = OmegaCommon::AssetBundle::open(bundlePath);
    if(bundleResult.isErr()){
        throw std::runtime_error("Failed to open app asset bundle (" + bundlePath.str() +
                                 "): " + bundleResult.error());
    }
    assetBundle = std::move(bundleResult.value());

    Composition::InitializeEngine();
    OMEGAWTK_DEBUG("Application Startup")
    Composition::FontEngine::Create();

    // Warm ICU's one-time lazy init on the main thread, at startup,
    // before any window paints. The cross-platform TextLayoutEngine
    // builds an OmegaCommon::BreakIterator on its first text layout,
    // and that first call is what triggers ICU's lazy global setup:
    // the default-locale cache, the line-break data load, and the
    // construction of ICU's internal std::mutex objects. Done lazily,
    // that all happens inside the first painted frame.
    //
    // Under PIX (and graphics debuggers generally), a GPU capture
    // suspends the app's threads; the very first ICU init then
    // deadlocks acquiring a low-level lock (CRT heap / loader / the
    // capture DLL's own) whose owner PIX has frozen. Forcing the init
    // here, single-threaded before the capturable render loop, leaves
    // every per-frame BreakIterator locking an already-constructed,
    // uncontended mutex — nothing for a capture to wedge against.
    // (Mirrors the Linux backend's FcInit() warm-up in
    // HarfBuzzFontEngine's ctor.) A no-op temporary; Type::Line still
    // runs Locale::getDefault() + createLineInstance() for empty text.
    { OmegaCommon::BreakIterator warm(OmegaCommon::BreakIterator::Type::Line,
                                      OmegaCommon::UniString{}); }
};

int AppInst::start(){
    // The native impls (CocoaApp::runEventLoop -> [NSApp run],
    // WinApp::runEventLoop -> GetMessage pump, GTKApp::runEventLoop
    // -> g_application_run) block here until terminate() asks them
    // to stop. After the loop returns, control flows back to the
    // caller's `omegaWTKMain` — which still holds SharedHandles to
    // its AppWindow locals. Those handles only drop when
    // `omegaWTKMain` returns, so framework teardown (FontEngine,
    // Composition pools, GTE device) must NOT run here: it would
    // close the D3D12 / Metal / Vulkan device while the test's
    // local AppWindow is still alive, and the AppWindow destructor
    // would then try to release backend resources against a freed
    // device (D3D12MA errors on Windows; harder to spot on macOS
    // where Metal silently nops). Teardown therefore runs in
    // ~AppInst, which is destroyed AFTER the caller's main returns
    // and after every user-held AppWindow has dropped its ref.
    return instance->ptr->runEventLoop();
};

void AppInst::terminate() {
    // Request-only. Asks the native loop to stop on its next turn;
    // the post-loop doShutdown() in start() handles the actual
    // teardown so it never races with in-flight platform callbacks.
    if(instance != nullptr && instance->ptr != nullptr){
        instance->ptr->terminate();
    }
};

void AppInst::doShutdown() {
    if(shutdownDone_){
        return;
    }
    shutdownDone_ = true;
    if(windowManager != nullptr){
        windowManager->closeAllWindows();
        windowManager.reset();
    }
    // Drain the process-wide Compositor BEFORE FontEngine /
    // CleanupEngine / OmegaGTE::Close. The Compositor is a Meyers
    // singleton (globalCompositor() in WidgetTreeHost.cpp) — its
    // destructor would otherwise run at atexit, after Close has
    // nulled gte.graphicsEngine and torn down the D3D12MA / Metal /
    // Vulkan allocator. The compositor's renderTargetStore +
    // windowSurfaces_ + observed LayerTrees still hold SharedHandles
    // to per-window backend buffers/textures and would release them
    // against a dead allocator, corrupting the heap (the
    // 556-Buffer / 6-Texture leak that surfaced via the ResourceTracker
    // churn dump). Calling shutdown() here drops every held
    // SharedHandle while the device is still alive; the dtor at
    // atexit then sees empty state and no-ops.
    Composition::shutdownGlobalCompositor();
    Composition::FontEngine::Destroy();
    Composition::CleanupEngine();
    OmegaGTE::Close(gte);
    OMEGAWTK_DEBUG("Application Shutdown")
};

// Native-Theme-Application-Plan Tier 1 (2026-06-30): OS theme cache +
// observer trampoline. The platform theme observer calls onThemeSet(...)
// with a freshly-queried ThemeDesc; we cache it and fan it out through
// the AppWindowManager's observer chain (which walks each window's
// widget tree via onThemeSetRecurse). Tier 1 is observational: this
// refreshes the cache and lets widgets react (e.g. Button re-queries
// its Light/Dark colors) — it does not yet drive the per-frame clear
// color (that is Tier 2's resolveWindowSurfaceColor).
const Native::ThemeDesc & AppInst::nativeTheme() const {
    return nativeTheme_;
}

void AppInst::onThemeSet(Native::ThemeDesc & desc) {
    nativeTheme_ = desc;
    if(windowManager != nullptr){
        windowManager->onThemeSet(desc);
    }
}

AppInst::~AppInst(){
    // Primary teardown site. By the time the destructor runs, every
    // user-held AppWindow SharedHandle has dropped (the caller's
    // `omegaWTKMain` returned, taking its locals with it), so the
    // windowManager owns the last refs and dropping it actually runs
    // each AppWindow destructor — releasing backend resources back
    // to the still-live GTE device. Only THEN do we shut down
    // Composition pools and close the device, in that order so
    // pool-held textures / pipelines / fences release before GTE
    // disappears underneath them.
    doShutdown();
    instance = nullptr;
};

OmegaCommon::FS::Path AppInst::executableDir(){
    // Platform "module path" lookup. CWD is unreliable (debugger /
    // ctest / different shell dir all leave it elsewhere than the EXE
    // dir); these APIs report the binary's own absolute path so callers
    // can locate adjacent assets unconditionally.
    return std::move(OmegaCommon::FS::getExecutableDir());
}

Native::NAP & AppInst::getNAP(){
    return ptr;
}

OmegaCommon::AssetBundle & AppInst::getAssetBundle(){
    return assetBundle;
}

const OmegaCommon::AssetBundle & AppInst::getAssetBundle() const{
    return assetBundle;
}

// ---------------------------------------------------------------
// Widget-View-Paint-Lifecycle-Plan Tier D / D7.1 (2026-06-04):
// process-wide active theme. Sheet rules reference theme variables
// via `StyleSheets::Var{name}`; the `StyleResolver` substitutes
// against `themeVars()` during the Style phase. Swapping the theme
// (`setThemeVars(...)`) dirties every known AppWindow's cascade so
// the next frame re-evaluates with the new bindings.
// ---------------------------------------------------------------

SharedHandle<ThemeVars> AppInst::themeVars() const {
    return themeVars_;
}

void AppInst::setThemeVars(SharedHandle<ThemeVars> theme){
    themeVars_ = std::move(theme);
    // Multi-window note: `AppWindowManager` currently only tracks
    // `rootWindow` — the `windows` vector / `addWindow` plumbing is
    // declared but not populated. When that ships, this is the call
    // site that needs to fan out across every tracked window. For
    // now, dirtying the root window is the complete coverage.
    if(windowManager == nullptr){
        return;
    }
    if(auto rootHandle = windowManager->getRootWindow(); rootHandle != nullptr){
        rootHandle->applyCascadeChange();
    }
}

};

#ifdef TARGET_WIN32 
#ifdef WINDOWS_PRIVATE

// void * create_hwnd_factory(void * hinst){
//     return __create_hwnd_factory(hinst);
// };

// void *hwnd_factory_get_all_hwnds(void *hwnd_factory){
//     return __hwnd_factory_get_all_hwnds(hwnd_factory);
// };

// void free_hwnd_factory(void *factory){
//     return __free_hwnd_factory(factory);
// };

RECT get_hwnd_item_coords(void * hwnd){
    return __get_hwnd_real_coords((HWND)hwnd);
};


#endif
#endif
