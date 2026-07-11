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

// TaskDialogIndirect lives in comctl32.lib (Common Controls v6), which is not
// among the default MSVC system libraries CMake links. Emit a linker directive
// so lld-link/link.exe pull it in for this TU.
#pragma comment(lib, "comctl32.lib")

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
            if(!SUCCEEDED(hr))
                std::cerr << "[WTK] WinFSDialog: CoCreateInstance failed (hr=0x"
                          << std::hex << (unsigned)hr << std::dec
                          << "); the file dialog will not appear." << std::endl;
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


    // An informational modal with a single OK button and no result. Built on
    // TaskDialogIndirect — the same proven API WinAlertDialog uses — rather than
    // a hand-rolled in-memory DLGTEMPLATE (which was fragile enough to produce an
    // access violation, a stack overflow, and a silent no-show across successive
    // fixes). The descriptor only carries title + body text, which is exactly
    // what a task dialog renders, so nothing is lost by the switch.
    class WinNoteDialog : public NativeNoteDialog {
    public:
        WinNoteDialog(const Descriptor & desc,NWH nativeWindow);
    };

    WinNoteDialog::WinNoteDialog(const Descriptor &desc,NWH nativeWindow):NativeNoteDialog(nativeWindow){
        // Present on construction, matching CocoaNoteDialog. Modal: blocks until
        // the user dismisses it (OK / Esc / close box).
        HWND hwnd = parentHwnd(parentWindow);

        std::wstring title = widen(desc.title);
        std::wstring content = widen(desc.str);

        TASKDIALOGCONFIG cfg{};
        cfg.cbSize = sizeof(cfg);
        cfg.hwndParent = hwnd;
        cfg.dwCommonButtons = TDCBF_OK_BUTTON;
        cfg.pszWindowTitle = title.c_str();
        cfg.pszMainInstruction = title.c_str();
        cfg.pszContent = content.c_str();
        cfg.pszMainIcon = TD_INFORMATION_ICON;

        HRESULT hr = TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
        if(!SUCCEEDED(hr))
            std::cerr << "[WTK] WinNoteDialog: TaskDialogIndirect failed (hr=0x"
                      << std::hex << (unsigned)hr << std::dec << ")" << std::endl;
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
