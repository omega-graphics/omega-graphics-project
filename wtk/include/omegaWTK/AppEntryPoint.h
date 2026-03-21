#ifndef OMEGAWTK_APP_ENTRY_POINT_H
#define OMEGAWTK_APP_ENTRY_POINT_H

#ifdef __cplusplus

namespace OmegaWTK {
class AppInst;
}

/// Creates an application instance. Returns an opaque pointer to AppInst.
/// Only include this header in app entry TUs to avoid pulling in full OmegaWTK.
extern "C" void* OmegaWTKCreateApp(int argc, char** argv);

/// Runs the app by calling the application-provided omegaWTKMain.
/// The app must define: int omegaWTKMain(OmegaWTK::AppInst* app);
extern "C" int OmegaWTKRunApp(void* app);

/// Destroys the application instance.
extern "C" void OmegaWTKDestroyApp(void* app);

#endif
#endif
