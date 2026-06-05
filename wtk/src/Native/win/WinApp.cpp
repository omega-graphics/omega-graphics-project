#include "omegaWTK/Native/NativeApp.h"
#include "NativePrivate/win/WinUtils.h"
#include <Windows.h>
#include <PathCch.h>
#include <shellapi.h>
// #include <atlstr.h>
#include <string>
#include <io.h>
#include <fcntl.h>

#include "HWNDFactory.h"

#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "pathcch.lib")
#pragma comment(lib, "shell32.lib")

namespace OmegaWTK::Native::Win {
    class WinApp : public NativeApp {
    public:
        WinApp(void *data){
            // On Win32 `data` is the HINSTANCE passed through from
            // WinMain (see wtk/target/win32/mmain.cpp). It is NOT a
            // NativeAppLaunchArgs * — that contract is the GTK / generic
            // entry-point path only. Win32 `commandLineArgs()` falls
            // back to CommandLineToArgvW below; we never adopt args
            // here.
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
            if(delegate_ != nullptr){
                delegate_->onAppWillTerminate();
            }
            PostQuitMessage(0);
        };
        int runEventLoop() override{
            // HACCEL hAccelTable = LoadAccelerators(hInstance,MAKEINTRESOURCE(IDC_@APPNAME@));
            if(delegate_ != nullptr){
                delegate_->onAppReady();
            }

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

        /// Win32 `WinMain` does not surface argc/argv; fall back to the
        /// wide command line + `CommandLineToArgvW` when the entry-point
        /// shim did not pass a `NativeAppLaunchArgs`. The first entry is
        /// the executable path (Win32 convention).
        ///
        /// `onOpenFile` / `onOpenURL` are intentionally not wired in v0:
        /// Win32 file associations launch a *new* process with the path
        /// as a command-line arg, and routing back into a running
        /// instance requires IPC (named pipes / DDE / COM). First-launch
        /// open is observable through this `commandLineArgs()` vector.
        OmegaCommon::Vector<OmegaCommon::String> commandLineArgs() const override {
            auto base = NativeApp::commandLineArgs();
            if(!base.empty()){
                return base;
            }
            OmegaCommon::Vector<OmegaCommon::String> out;
            int argcW = 0;
            LPWSTR *argvW = CommandLineToArgvW(GetCommandLineW(), &argcW);
            if(argvW == nullptr){
                return out;
            }
            out.reserve(static_cast<std::size_t>(argcW));
            for(int i = 0; i < argcW; ++i){
                int needed = WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1,
                                                  nullptr, 0, nullptr, nullptr);
                if(needed <= 0){
                    out.emplace_back("");
                    continue;
                }
                std::string buf(static_cast<std::size_t>(needed - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1,
                                     buf.data(), needed, nullptr, nullptr);
                out.emplace_back(std::move(buf));
            }
            LocalFree(argvW);
            return out;
        }

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
