#include "omegaWTK/Native/NativeApp.h"


namespace OmegaWTK::Native {

NativeApp::NativeApp(){

};

void NativeApp::setDelegate(NativeAppDelegate * delegate){
    delegate_ = delegate;
}

const NativeAppLaunchArgs & NativeApp::launchArgs() const {
    return launchArgs_;
}

OmegaCommon::Vector<OmegaCommon::String> NativeApp::commandLineArgs() const {
    OmegaCommon::Vector<OmegaCommon::String> args;
    if(launchArgs_.argv == nullptr || launchArgs_.argc <= 0){
        return args;
    }
    args.reserve(static_cast<std::size_t>(launchArgs_.argc));
    for(int i = 0; i < launchArgs_.argc; ++i){
        const char * a = launchArgs_.argv[i];
        args.emplace_back(a ? a : "");
    }
    return args;
}

void NativeApp::adoptLaunchArgs(const NativeAppLaunchArgs & args){
    launchArgs_ = args;
}


};

#ifdef TARGET_WIN32

#include "win/HWNDFactory.h"
#include "NativePrivate/win/HWNDItem.h"

RECT __get_hwnd_real_coords(HWND hwnd){
    auto *item = (OmegaWTK::Native::Win::HWNDItem *)OmegaWTK::Native::Win::getHWNDUserData(hwnd);
    RECT rc;
    auto & _rect = item->wndrect;
    rc.left = LONG(_rect.pos.x);
    rc.right = LONG(_rect.pos.x + _rect.w);
    rc.bottom = LONG(_rect.pos.y);
    rc.top = LONG(_rect.pos.y + _rect.h);
    return rc;
};

#endif
;
