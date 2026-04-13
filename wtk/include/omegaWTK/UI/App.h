#include "omegaWTK/Core/Core.h"
#include "omega-common/assets.h"

#ifndef OMEGAWTK_UI_APP_H
#define OMEGAWTK_UI_APP_H


namespace OmegaWTK {
    namespace Native {
        class NativeApp;
        typedef SharedHandle<NativeApp> NAP;
    };

class AppWindowManager;

class OMEGAWTK_EXPORT AppInst {
    Native::NAP ptr;
    static AppInst *instance;
    OmegaCommon::Optional<OmegaCommon::AssetBundle> assetBundle;
public:
    OMEGACOMMON_CLASS("OmegaWTK.AppInst")

    
    static  AppInst * inst();
    UniqueHandle<AppWindowManager> windowManager;

    explicit AppInst(void *data);

    static int start();

    static void terminate();

   
// #ifdef TARGET_WIN32
//     AppInst(void * windows_inst);
// #endif
    Native::NAP & getNAP();
    OmegaCommon::AssetBundle * getAssetBundle();
    const OmegaCommon::AssetBundle * getAssetBundle() const;
    ~AppInst();
};


    
};

// #ifdef TARGET_WIN32 
// #ifdef WINDOWS_PRIVATE

// OMEGAWTK_EXPORT RECT get_hwnd_item_coords(void * hwnd);

// #endif
// #endif

#endif
