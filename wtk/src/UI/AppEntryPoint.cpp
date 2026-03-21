#include "omegaWTK/AppEntryPoint.h"
#include "omegaWTK/Main.h"
#include "omegaWTK/UI/App.h"
#include "omegaWTK/Native/NativeApp.h"

extern "C" void* OmegaWTKCreateApp(int argc, char** argv) {
    OmegaWTK::Native::NativeAppLaunchArgs launchArgs{argc, argv};
    return new OmegaWTK::AppInst(&launchArgs);
}

extern "C" int OmegaWTKRunApp(void* app) {
    return omegaWTKMain(static_cast<OmegaWTK::AppInst*>(app));
}

extern "C" void OmegaWTKDestroyApp(void* app) {
    delete static_cast<OmegaWTK::AppInst*>(app);
}
