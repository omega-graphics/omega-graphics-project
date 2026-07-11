//#include "resource.h"
#include <omegaWTK/UI/App.h>
#include <omegaWTK/Main.h>

#include <Windows.h>
#include <wrl.h>

#define MAX_LOADSTRING 100

// LPCSTR szTitle = "@APPNAME@";                
// LPCSTR szWindowClass = "@APPNAME@"; 
static const WORD MAX_CONSOLE_LINES = 500;

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,  PSTR lpCmdLine, int nShowCmd){

    HRESULT hr;
    
    // STA (apartment-threaded) is required on the UI thread: the Windows
    // Common Item Dialog (IFileOpenDialog/IFileSaveDialog), OLE drag/drop and
    // the clipboard all fail on an MTA thread. Background render threads that
    // want the MTA initialize their own apartment (see CompositorFrameWorker).
    hr = CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    if(!SUCCEEDED(hr))
    {
        //Handle Error!
    }
   
    auto *appInst = new OmegaWTK::AppInst((void *)hInstance);
   
   
    int returnCode = omegaWTKMain(appInst);

    CoUninitialize();

    delete appInst;


    return returnCode;

};