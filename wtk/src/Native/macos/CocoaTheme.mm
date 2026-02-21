#include "omegaWTK/Native/NativeTheme.h"

#import <AppKit/AppKit.h>

namespace OmegaWTK::Native {
    ThemeDesc queryCurrentTheme(){
        ThemeDesc desc {};
        (void)NSApp.appearance;
        return desc;
    }
}
