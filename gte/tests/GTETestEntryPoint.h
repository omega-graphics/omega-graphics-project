#ifndef GTETESTENTRYPOINT_H
#define GTETESTENTRYPOINT_H

// Shared GTE tests compile one source for every backend. The D3D12 test
// executables are linked through WinMainCRTStartup (see
// gte/tests/directx/CMakeLists.txt), so their user entry point must be
// WinMain; every other backend uses a normal main.
//
// GTE_TEST_ENTRY_POINT hides that split while still handing the test body the
// usual (int argc, const char *argv[]) on every backend, so a single body can
// branch on command-line arguments (e.g. the "negative" sub-case). On Windows
// the arguments come from the CRT globals __argc / __argv, which
// WinMainCRTStartup populates before it calls WinMain.

#ifdef TARGET_DIRECTX
#include <Windows.h>
#include <cstdlib>  // __argc, __argv

#define GTE_TEST_ENTRY_POINT                                            \
    static int gteTestMain(int argc, const char *argv[]);              \
    int APIENTRY WinMain(HINSTANCE, HINSTANCE, PSTR, int) {            \
        return gteTestMain(__argc, const_cast<const char **>(__argv)); \
    }                                                                  \
    static int gteTestMain(int argc, const char *argv[])
#else
#define GTE_TEST_ENTRY_POINT int main(int argc, const char *argv[])
#endif

#endif
