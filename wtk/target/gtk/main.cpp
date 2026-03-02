#include <OmegaWTK.h>

int main(int argc,char *argv[]){
    OmegaWTK::Native::NativeAppLaunchArgs launchArgs {argc,argv};
    auto * app = new OmegaWTK::AppInst(&launchArgs);
    auto rc = omegaWTKMain(app);
    delete app;
    return rc;
};
