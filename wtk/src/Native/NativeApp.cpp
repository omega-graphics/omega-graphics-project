#include "omegaWTK/Native/NativeApp.h"


namespace OmegaWTK::Native {

NativeApp::NativeApp(){
    
};


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
