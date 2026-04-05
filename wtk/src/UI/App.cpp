#include "omegaWTK/UI/App.h"
#include "omegaWTK/Composition/FontEngine.h"

#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/UI/AppWindow.h"

#include "omega-common/assets.h"

#include "omegaWTK/Native/NativeApp.h"

namespace OmegaWTK {



AppInst * AppInst::instance;

AppInst *AppInst::inst() {
    return instance;
}

AppInst::AppInst(void *data):ptr(Native::make_native_app(data)),windowManager(std::make_unique<AppWindowManager>()){
    instance = this;
    gte = OmegaGTE::InitWithDefaultDevice();
    Composition::InitializeEngine();
    OMEGAWTK_DEBUG("Application Startup")
    Composition::FontEngine::Create();
    /// Load your app's assets here. 
    OmegaCommon::FS::Path assets_path("./assets.omxa");
    if(assets_path.exists())
        OmegaCommon::AssetLibrary::loadAssetFile(assets_path);
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

Native::NAP & AppInst::getNAP(){
    return ptr;
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
