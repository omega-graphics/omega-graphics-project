#include "omegaWTK/Native/NativeApp.h"
#include "NativePrivate/win/WinUtils.h"
#include <Windows.h>
#include <PathCch.h>
// #include <atlstr.h>
#include <string>
#include <io.h>
#include <fcntl.h>

#include "HWNDFactory.h"

#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "pathcch.lib")

namespace OmegaWTK::Native::Win {
    class WinApp : public NativeApp {
    public:
        WinApp(void *data){
            HWNDFactory *factory = new HWNDFactory((HINSTANCE)data);
            HWNDFactory::appFactoryInst = factory;
            /**
             Set current directory to executable's dir.
            */
           
            HRESULT hr;
            LPTSTR exec_string = GetCommandLineA();
            OmegaCommon::String exec_string_cpp (exec_string);
           std::wstring ws;
           cpp_str_to_cpp_wstr(exec_string_cpp,ws);
           
            hr = PathCchRemoveFileSpec(ws.data(),ws.size());
            if(FAILED(hr)){

            };
            // MessageBoxW(HWND_DESKTOP,ws.data(),L"NOTE",MB_OK);
            SetCurrentDirectoryW(ws.data());
        };
        void terminate() override{
            PostQuitMessage(0);
        };
        int runEventLoop() override{
            // HACCEL hAccelTable = LoadAccelerators(hInstance,MAKEINTRESOURCE(IDC_@APPNAME@));

            MSG msg = {};
            while (GetMessage(&msg,NULL,0,0))
            {
                if(msg.message == WM_QUIT){
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
              
            }
            return msg.wParam;
        };
        ~WinApp(){
            delete HWNDFactory::appFactoryInst;
        };
    };
    

};

namespace OmegaWTK::Native {
    NAP make_native_app(void *data){
        return (NAP)new Win::WinApp(data);
    };
}