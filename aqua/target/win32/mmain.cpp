#include <aqua/App.h>

#include <windows.h>
#include <objbase.h>

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, int nShowCmd) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (!SUCCEEDED(hr)) {
        return 1;
    }

    auto app = Aqua::CreateApp();
    int rc = 0;
    app->run();

    CoUninitialize();
    return rc;
}
