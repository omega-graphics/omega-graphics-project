#include "omegaWTK/Native/NativeDialog.h"
#include "NativePrivate/win/HWNDItem.h"
#include "HWNDFactory.h"
#include "WinAppWindow.h"
#include <combaseapi.h>
#include <iostream>
#include <string>
#include <vector>
#include <cctype>

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

    static std::wstring widen(const OmegaCommon::String & s) {
        if(s.empty()) return std::wstring();
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), len);
        return w;
    }

    static OmegaCommon::String narrow(LPCWSTR s) {
        if(!s) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
        if(len <= 1) return {};
        OmegaCommon::String out(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), len, nullptr, nullptr);
        return out;
    }

    static HWND parentHwnd(const NWH & parentWindow){
        auto item = std::dynamic_pointer_cast<HWNDItem>(parentWindow);
        return item ? item->hwnd : nullptr;
    }

     class WinFSDialog : public NativeFSDialog {
        bool read_or_write;
        bool allowMultiple;
        OmegaCommon::String openLocation;
        OmegaCommon::Vector<FileFilter> filters;
        IFileOpenDialog * dialog_ty_1 = nullptr;
        IFileSaveDialog * dialog_ty_2 = nullptr;
        OmegaCommon::Promise<OmegaCommon::Vector<OmegaCommon::FS::Path>> result;

        // Reads the file-system path out of an IShellItem and appends it.
        static void appendItem(IShellItem * item, OmegaCommon::Vector<OmegaCommon::FS::Path> & out){
            if(!item) return;
            PWSTR path = nullptr;
            if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path){
                out.push_back(OmegaCommon::FS::Path(narrow(path)));
                CoTaskMemFree(path);
            }
        }

    public:
        WinFSDialog(const Descriptor & desc,NWH nativeWindow):
            NativeFSDialog(nativeWindow),
            read_or_write(desc.type == Read),
            allowMultiple(desc.allowMultiple),
            filters(desc.filters){
            OmegaCommon::FS::Path loc = desc.openLocation;
            openLocation = loc.str();
            HRESULT hr;
            if(read_or_write)
                hr = CoCreateInstance(CLSID_FileOpenDialog,NULL,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog_ty_1));
            else
                hr = CoCreateInstance(CLSID_FileSaveDialog,NULL,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog_ty_2));
            (void)hr;
        };
        ~WinFSDialog();
        OmegaCommon::Async<OmegaCommon::Vector<OmegaCommon::FS::Path>> getResult() override;
    };

    class WinAlertDialog : public NativeAlertDialog {
        Descriptor desc;
        OmegaCommon::Promise<Result> result;

        static Result resultForLabel(const OmegaCommon::String & label, bool isFirst){
            OmegaCommon::String l;
            for(char c : label) l.push_back((char)std::tolower((unsigned char)c));
            if(l == "ok")     return Result::OK;
            if(l == "cancel") return Result::Cancel;
            if(l == "yes")    return Result::Yes;
            if(l == "no")     return Result::No;
            return isFirst ? Result::OK : Result::Cancel;
        }
    public:
        WinAlertDialog(const Descriptor & desc,NWH nativeWindow):
            NativeAlertDialog(nativeWindow),desc(desc){};
        OmegaCommon::Async<Result> getResult() override;
    };

    OmegaCommon::Async<OmegaCommon::Vector<OmegaCommon::FS::Path>> WinFSDialog::getResult(){
        HWND hwnd = parentHwnd(parentWindow);
        OmegaCommon::Vector<OmegaCommon::FS::Path> out;

        // Build the filter specs; keep the wide strings alive across Show().
        std::vector<std::wstring> backing;
        backing.reserve(filters.size() * 2);
        std::vector<COMDLG_FILTERSPEC> specs;
        for(auto & f : filters){
            std::wstring spec;
            for(size_t i = 0; i < f.extensions.size(); ++i){
                if(i) spec += L";";
                spec += L"*." + widen(f.extensions[i]);
            }
            if(spec.empty()) spec = L"*.*";
            backing.push_back(widen(f.label));
            backing.push_back(spec);
            COMDLG_FILTERSPEC fs{ backing[backing.size()-2].c_str(), backing[backing.size()-1].c_str() };
            specs.push_back(fs);
        }

        IFileDialog * dialog = read_or_write ? (IFileDialog *)dialog_ty_1 : (IFileDialog *)dialog_ty_2;
        if(dialog){
            if(!specs.empty())
                dialog->SetFileTypes((UINT)specs.size(), specs.data());
            if(!openLocation.empty()){
                IShellItem * folder = nullptr;
                if(SUCCEEDED(SHCreateItemFromParsingName(widen(openLocation).c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder){
                    dialog->SetFolder(folder);
                    folder->Release();
                }
            }
        }

        if(read_or_write && dialog_ty_1){
            DWORD opts = 0;
            dialog_ty_1->GetOptions(&opts);
            if(allowMultiple) opts |= FOS_ALLOWMULTISELECT;
            dialog_ty_1->SetOptions(opts);
            if(SUCCEEDED(dialog_ty_1->Show(hwnd))){
                IShellItemArray * items = nullptr;
                if(SUCCEEDED(dialog_ty_1->GetResults(&items)) && items){
                    DWORD count = 0;
                    items->GetCount(&count);
                    for(DWORD i = 0; i < count; ++i){
                        IShellItem * item = nullptr;
                        if(SUCCEEDED(items->GetItemAt(i, &item)) && item){
                            appendItem(item, out);
                            item->Release();
                        }
                    }
                    items->Release();
                }
            }
        }
        else if(!read_or_write && dialog_ty_2){
            if(SUCCEEDED(dialog_ty_2->Show(hwnd))){
                IShellItem * item = nullptr;
                if(SUCCEEDED(dialog_ty_2->GetResult(&item)) && item){
                    appendItem(item, out);
                    item->Release();
                }
            }
        }

        result.set(out);
        return result.async();
    }

    WinFSDialog::~WinFSDialog(){
        if(dialog_ty_1 != nullptr)
            dialog_ty_1->Release();
        if(dialog_ty_2 != nullptr)
            dialog_ty_2->Release();
    };

    OmegaCommon::Async<NativeAlertDialog::Result> WinAlertDialog::getResult(){
        HWND hwnd = parentHwnd(parentWindow);

        std::wstring title = widen(desc.title);
        std::wstring message = widen(desc.message);

        TASKDIALOGCONFIG cfg{};
        cfg.cbSize = sizeof(cfg);
        cfg.hwndParent = hwnd;
        cfg.pszWindowTitle = title.c_str();
        cfg.pszMainInstruction = title.c_str();
        cfg.pszContent = message.c_str();
        switch(desc.style){
            case Style::Info:    cfg.pszMainIcon = TD_INFORMATION_ICON; break;
            case Style::Warning: cfg.pszMainIcon = TD_WARNING_ICON; break;
            case Style::Error:   cfg.pszMainIcon = TD_ERROR_ICON; break;
        }

        // Custom buttons get ids starting at 100 so they don't collide with
        // the IDOK (1) used by the empty-labels default.
        const int kBaseId = 100;
        std::vector<std::wstring> labelBacking;
        std::vector<TASKDIALOG_BUTTON> buttons;
        labelBacking.reserve(desc.buttonLabels.size());
        for(size_t i = 0; i < desc.buttonLabels.size(); ++i){
            labelBacking.push_back(widen(desc.buttonLabels[i]));
            TASKDIALOG_BUTTON b{ kBaseId + (int)i, labelBacking.back().c_str() };
            buttons.push_back(b);
        }
        if(buttons.empty()){
            cfg.dwCommonButtons = TDCBF_OK_BUTTON;
        } else {
            cfg.pButtons = buttons.data();
            cfg.cButtons = (UINT)buttons.size();
        }

        int clicked = 0;
        Result res = Result::Cancel;
        if(SUCCEEDED(TaskDialogIndirect(&cfg, &clicked, nullptr, nullptr))){
            if(desc.buttonLabels.empty()){
                res = (clicked == IDOK) ? Result::OK : Result::Cancel;
            } else {
                int index = clicked - kBaseId;
                if(index >= 0 && index < (int)desc.buttonLabels.size())
                    res = resultForLabel(desc.buttonLabels[index], index == 0);
            }
        }
        result.set(res);
        return result.async();
    }


    class WinNoteDialog : public NativeNoteDialog {
        static INT_PTR DlgProc(HWND , UINT, WPARAM, LPARAM);
        HGLOBAL hgbl;
        public:
        WinNoteDialog(const Descriptor & desc,NWH nativeWindow);
        ~WinNoteDialog();
        void show();
        void close();
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
        auto message_str = OmegaCommon::UniString::fromUTF8(desc.title.c_str());
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

        auto msg_ptr = reinterpret_cast<const WCHAR *>(message_str.getBuffer());

        for (wstr = (LPWSTR)lpw;(*wstr++ = *msg_ptr++) != 0;);
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
        return (SharedHandle<NativeFSDialog>)new Win::WinFSDialog(desc,nativeWindow);
    }
    SharedHandle<NativeAlertDialog> NativeAlertDialog::Create(const Descriptor &desc, NWH nativeWindow){
        return (SharedHandle<NativeAlertDialog>)new Win::WinAlertDialog(desc,nativeWindow);
    }
     SharedHandle<NativeNoteDialog> NativeNoteDialog::Create(const Descriptor &desc, NWH nativeWindow){
        return (SharedHandle<NativeNoteDialog>)new Win::WinNoteDialog(desc,nativeWindow);

    };
}
