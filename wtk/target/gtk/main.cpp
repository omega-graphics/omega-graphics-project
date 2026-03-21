#include <omegaWTK/UI/App.h>
#include <omegaWTK/Native/NativeApp.h>
#include <omegaWTK/Main.h>

int main(int argc,char *argv[]){
    OmegaWTK::Native::NativeAppLaunchArgs launchArgs {argc,argv};
    auto * app = new OmegaWTK::AppInst(&launchArgs);
    auto rc = omegaWTKMain(app);
    delete app;
    return rc;
};
