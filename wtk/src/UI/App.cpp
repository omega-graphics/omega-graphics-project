#include "omegaWTK/UI/App.h"
#include "omegaWTK/Composition/FontEngine.h"
#include "omegaWTK/Core/GTEHandle.h"

#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/UI/ThemeVars.h"

#include "omega-common/assets.h"

#include "omegaWTK/Native/NativeApp.h"

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
    gte = OmegaGTE::InitWithDefaultDevice();

    /// Load your app's assets here. 
    OmegaCommon::FS::Path assets_path("./default.pak");
    if(assets_path.exists()){
        auto bundleResult = OmegaCommon::AssetBundle::open(assets_path);
        if(bundleResult.isOk()){
            assetBundle = std::move(bundleResult.value());
        }
    }

    Composition::InitializeEngine();
    OMEGAWTK_DEBUG("Application Startup")
    Composition::FontEngine::Create();
    
};

int AppInst::start(){
    return instance->ptr->runEventLoop();
};

void AppInst::terminate() {
    instance->windowManager->closeAllWindows();
    instance->windowManager.reset();
    Composition::FontEngine::Destroy();
    Composition::CleanupEngine();
    OmegaGTE::Close(gte);
    OMEGAWTK_DEBUG("Application Shutdown")
    instance->ptr->terminate();
};

// void AppInst::onThemeSet(Native::ThemeDesc &desc) {

// }

AppInst::~AppInst(){
    // Cleanup may already have been performed by terminate().
    // Guard against double-cleanup.
    if(windowManager != nullptr){
        windowManager->closeAllWindows();
        windowManager.reset();
        Composition::FontEngine::Destroy();
        Composition::CleanupEngine();
        OmegaGTE::Close(gte);
    }
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

OmegaCommon::AssetBundle * AppInst::getAssetBundle(){
    if(!assetBundle.has_value()){
        return nullptr;
    }
    return &(*assetBundle);
}

const OmegaCommon::AssetBundle * AppInst::getAssetBundle() const{
    if(!assetBundle.has_value()){
        return nullptr;
    }
    return &(*assetBundle);
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
