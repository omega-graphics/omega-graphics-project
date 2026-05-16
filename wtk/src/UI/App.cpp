#include "omegaWTK/UI/App.h"
#include "omegaWTK/Composition/FontEngine.h"
#include "omegaWTK/Core/GTEHandle.h"

#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/UI/AppWindow.h"

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
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if(n == 0 || n == MAX_PATH){
        return OmegaCommon::FS::Path("");
    }
    std::string path(buf, n);
    const auto slash = path.find_last_of("\\/");
    if(slash == std::string::npos){
        return OmegaCommon::FS::Path("");
    }
    return OmegaCommon::FS::Path(path.substr(0, slash));
#elif defined(__APPLE__)
    char buf[PATH_MAX] = {};
    uint32_t size = sizeof(buf);
    if(_NSGetExecutablePath(buf, &size) != 0){
        return OmegaCommon::FS::Path("");
    }
    std::string path(buf);
    const auto slash = path.find_last_of('/');
    if(slash == std::string::npos){
        return OmegaCommon::FS::Path("");
    }
    return OmegaCommon::FS::Path(path.substr(0, slash));
#else
    char buf[PATH_MAX] = {};
    ssize_t n = readlink("/proc/self/exe", buf, PATH_MAX - 1);
    if(n <= 0){
        return OmegaCommon::FS::Path("");
    }
    std::string path(buf, (size_t)n);
    const auto slash = path.find_last_of('/');
    if(slash == std::string::npos){
        return OmegaCommon::FS::Path("");
    }
    return OmegaCommon::FS::Path(path.substr(0, slash));
#endif
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
