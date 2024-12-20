#include "omegaWTK/Native/NativeDialog.h"
#include "NativePrivate/win/HWNDItem.h"
#include "HWNDFactory.h"
#include "WinAppWindow.h"
#include <combaseapi.h>
#include <iostream>

#include <windows.h>
#include <ShlObj_core.h>
#include <CommCtrl.h>
#include <ShObjIdl.h>
// #include <atlbase.h>
#include <windowsx.h>
#include <winnt.h>
#include <wtypesbase.h>
// #include <atlstr.h>

namespace OmegaWTK::Native::Win {

     class WinFSDialog : public NativeFSDialog {
        bool read_or_write;
        IFileOpenDialog * dialog_ty_1;
        IFileSaveDialog * dialog_ty_2;
        void close();
        void show();
        OmegaCommon::Promise<OmegaCommon::String> result;
    public:
        WinFSDialog(bool read_or_write,NWH nativeWindow);
        ~WinFSDialog();
        OmegaCommon::Async<OmegaCommon::String> getResult() override;
    };

    class WinNoteDialog : public NativeNoteDialog {
        static INT_PTR DlgProc(HWND , UINT, WPARAM, LPARAM);
        HGLOBAL hgbl;
        public:
        WinNoteDialog(const Descriptor & desc,NWH nativeWindow);
        ~WinNoteDialog();
        void show();
        void close();
    };


    WinFSDialog::WinFSDialog(bool read_or_write,NWH nativeWindow):NativeFSDialog(nativeWindow),read_or_write(read_or_write){
        HRESULT hr;
        if(read_or_write)
            hr = CoCreateInstance(CLSID_FileOpenDialog,NULL,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog_ty_1));
        else 
           hr = CoCreateInstance(CLSID_FileSaveDialog,NULL,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog_ty_2));

        
    };

    void WinFSDialog::close(){
        HRESULT hr;
        if(read_or_write){
            hr = dialog_ty_1->Close(S_OK);
        }
    };

    OmegaCommon::Async<OmegaCommon::String> WinFSDialog::getResult(){
        return result.async();
    }


    void WinFSDialog::show(){
        HRESULT hr;
        if(read_or_write){
            hr = dialog_ty_1->Show(std::dynamic_pointer_cast<HWNDItem>(parentWindow)->hwnd);
        }
        else {
             hr = dialog_ty_2->Show(std::dynamic_pointer_cast<HWNDItem>(parentWindow)->hwnd);
        };
    };

    WinFSDialog::~WinFSDialog(){
        if(dialog_ty_1 != nullptr)
            dialog_ty_1->Release();
        else 
            dialog_ty_2->Release();
    };

    LPWORD lpwAlign(LPWORD lpIn)
    {
        ULONG ul;
        
        ul = (ULONG)lpIn;
        ul ++;
        ul >>=2;
        ul <<=2;
        return (LPWORD)ul;
    }

    INT_PTR CALLBACK WinNoteDialog::DlgProc(HWND hDlg,UINT msg,WPARAM wParam,LPARAM lParam){
        /// LParam is ptr to WinNoteDialog!
        INT_PTR res = FALSE;
        switch (msg) {
            case WM_INITDIALOG: {
                // SetWindowLongPtrA(hDlg,DWLP_USER,lParam);
                res = TRUE;
                break;
            }
            case WM_COMMAND: {
                if (LOWORD(wParam) == IDOK){
                    EndDialog(hDlg, LOWORD(wParam));
                    res = TRUE;
                };
                break;
            }
            default: {
                return DefDlgProcA(hDlg,msg,wParam,lParam);
                break;
            }
        }
        return res;
    };

    #define ID_TEXT 4

    WinNoteDialog::WinNoteDialog(const Descriptor &desc,NWH nativeWindow):NativeNoteDialog(nativeWindow){
        icu::UnicodeString message_str(desc.title.c_str());
        LPDLGTEMPLATE lpdt;
        LPDLGITEMTEMPLATE lpdtItem;
        LPWORD lpw;
        LPWSTR wstr;
        UINT nchar;
        hgbl = GlobalAlloc(GMEM_ZEROINIT,1024);
        if (!hgbl) {
            std::cout << "Failed to Allocate Mem For Template" << std::endl;
            MessageBoxA(GetForegroundWindow(),"Failed to Allocate Mem For Template",NULL,MB_OK);
            exit(1);
        }
        MessageBoxA(GetForegroundWindow(),"Locking HGLOBAL",NULL,MB_OK);
        lpdt = (LPDLGTEMPLATE)GlobalLock(hgbl);
        lpdt->style = WS_POPUP | WS_BORDER | WS_SYSMENU | DS_MODALFRAME | WS_CAPTION;
        lpdt->cdit = 2;
        lpdt->x  = 10;  lpdt->y  = 10;
        lpdt->cx = 200; lpdt->cy = 200;
        lpw = (LPWORD)(lpdt + 1);
        *lpw++ = 0;             // No menu
        *lpw++ = 0; 


        wstr = (LPWSTR)lpw;
        nchar = 1 + MultiByteToWideChar(CP_ACP, 0,desc.title.c_str(), -1,wstr,50);
        lpw += nchar;


         MessageBoxA(GetForegroundWindow(),"Created Dialog Frame",NULL,MB_OK);
        /**
         Define an OK button.
        */
        lpw = lpwAlign(lpw);    // Align DLGITEMTEMPLATE on DWORD boundary
        MessageBoxA(GetForegroundWindow(),"Aligned DWORD ",NULL,MB_OK);
        lpdtItem = (LPDLGITEMTEMPLATE)lpw;
        lpdtItem->x  = 10; 
        MessageBoxA(GetForegroundWindow(),"Getting DLG Item Template and Setting First Vals ",NULL,MB_OK);
        lpdtItem->y  = 10;
        lpdtItem->cx = 80; lpdtItem->cy = 20;
        lpdtItem->id = IDOK;       // OK button identifier
        lpdtItem->style = WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON;

        MessageBoxA(GetForegroundWindow(),"Setting DLG Item Template ",NULL,MB_OK);

        lpw = (LPWORD)(lpdtItem + 1);
        *lpw++ = 0xFFFF;
        *lpw++ = 0x0080;        // Button class

         MessageBoxA(GetForegroundWindow(),"Created Button Without Text",NULL,MB_OK);

        wstr = (LPWSTR)lpw;
        nchar = 1 + MultiByteToWideChar(CP_ACP, 0, "OK", -1, wstr, 50);
        lpw += nchar;
        *lpw++ = 0;  

        MessageBoxA(GetForegroundWindow(),"Created OK Button",NULL,MB_OK);

        lpw = lpwAlign(lpw);    // Align DLGITEMTEMPLATE on DWORD boundary
        lpdtItem = (LPDLGITEMTEMPLATE)lpw;
        lpdtItem->x  = 10; lpdtItem->y  = 10;
        lpdtItem->cx = 40; lpdtItem->cy = 20;
        lpdtItem->id = ID_TEXT;    // Text identifier
        lpdtItem->style = WS_CHILD | WS_VISIBLE | SS_LEFT;

        lpw = (LPWORD)(lpdtItem + 1);
        *lpw++ = 0xFFFF;
        *lpw++ = 0x0082;        // Static class

        LPWSTR msg_ptr = (LPWSTR)message_str.getBuffer();

        for (wstr = (LPWSTR)lpw;*wstr++ = (WCHAR)*msg_ptr++;);
        lpw = (LPWORD)wstr;
        *lpw++ = 0;     

        MessageBoxA(GetForegroundWindow(),"Created Description Text",NULL,MB_OK);

        GlobalUnlock(hgbl); 
        MessageBoxA(GetForegroundWindow(),"Unlocked HGLOBAL",NULL,MB_OK);
    };

    WinNoteDialog::~WinNoteDialog(){
        GlobalFree(hgbl); 
    };



    void WinNoteDialog::show(){
        DialogBoxIndirectA(HWNDFactory::appFactoryInst->hInst,(LPDLGTEMPLATE)hgbl,std::dynamic_pointer_cast<HWNDItem>(parentWindow)->hwnd,WinNoteDialog::DlgProc);
    };
}

namespace OmegaWTK::Native {
    SharedHandle<NativeFSDialog> NativeFSDialog::Create(const Descriptor &desc, NWH nativeWindow){
        auto is_read_or_write = desc.type == Read;
        auto ptr = new Win::WinFSDialog(is_read_or_write,nativeWindow);
        return (SharedHandle<NativeFSDialog>)ptr;
    }
     SharedHandle<NativeNoteDialog> NativeNoteDialog::Create(const Descriptor &desc, NWH nativeWindow){
        return (SharedHandle<NativeNoteDialog>)new Win::WinNoteDialog(desc,nativeWindow);
        
    };
}