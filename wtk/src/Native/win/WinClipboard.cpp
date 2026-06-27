#include "omegaWTK/Native/NativeClipboard.h"

#include <Windows.h>
#include <shlobj.h>   // DROPFILES, DragQueryFileW
#include <shellapi.h> // CF_HDROP

#include <cstring>
#include <memory>
#include <string>

namespace OmegaWTK::Native::Win {

namespace {

/// UTF-8 (OmegaCommon::String) -> UTF-16 (Win32 wide).
std::wstring utf8ToWide(const OmegaCommon::String & s){
    if(s.empty()){
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if(n <= 0){
        return {};
    }
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

/// UTF-16 -> UTF-8. `len` is the source length in wchar_t, or -1 when the
/// source is NUL-terminated (in which case the result's trailing NUL is
/// trimmed).
OmegaCommon::String wideToUtf8(const wchar_t *w, int len){
    if(w == nullptr){
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if(n <= 0){
        return {};
    }
    OmegaCommon::String s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], n, nullptr, nullptr);
    if(len == -1 && !s.empty() && s.back() == '\0'){
        s.pop_back();
    }
    return s;
}

} // namespace

/// Win32 system clipboard via the OpenClipboard / GetClipboardData API.
/// Text rides CF_UNICODETEXT; file lists ride CF_HDROP (the format an
/// Explorer copy produces and a paste-target expects).
///
/// Compile status: source-complete, compile-unverified off-platform
/// (this Linux host cannot build the Win32 backend). See the §2.6 note
/// in Native-API-Completion-Proposal.md.
class WinClipboard : public NativeClipboard {
public:
    WinClipboard() = default;
    ~WinClipboard() override = default;

    bool hasType(ClipboardDataType type) const override {
        switch(type){
            case ClipboardDataType::PlainText:
                return IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
            case ClipboardDataType::HTML:
                return IsClipboardFormatAvailable(htmlFormat()) != 0;
            case ClipboardDataType::Image:
                return IsClipboardFormatAvailable(CF_DIB) != 0
                    || IsClipboardFormatAvailable(CF_BITMAP) != 0;
            case ClipboardDataType::FilePaths:
                return IsClipboardFormatAvailable(CF_HDROP) != 0;
        }
        return false;
    }

    OmegaCommon::String getText() const override {
        if(IsClipboardFormatAvailable(CF_UNICODETEXT) == 0){
            return {};
        }
        if(OpenClipboard(nullptr) == 0){
            return {};
        }
        OmegaCommon::String out;
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if(h != nullptr){
            const wchar_t *w = static_cast<const wchar_t *>(GlobalLock(h));
            if(w != nullptr){
                out = wideToUtf8(w, -1);
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
        return out;
    }

    void setText(const OmegaCommon::String & text) override {
        if(OpenClipboard(nullptr) == 0){
            return;
        }
        EmptyClipboard();
        std::wstring w = utf8ToWide(text);
        SIZE_T bytes = (w.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if(h != nullptr){
            void *dst = GlobalLock(h);
            if(dst != nullptr){
                std::memcpy(dst, w.c_str(), bytes);
                GlobalUnlock(h);
                // Ownership of `h` transfers to the clipboard on success.
                if(SetClipboardData(CF_UNICODETEXT, h) == nullptr){
                    GlobalFree(h);
                }
            } else {
                GlobalFree(h);
            }
        }
        CloseClipboard();
    }

    OmegaCommon::Vector<OmegaCommon::FS::Path> getFilePaths() const override {
        OmegaCommon::Vector<OmegaCommon::FS::Path> out;
        if(IsClipboardFormatAvailable(CF_HDROP) == 0){
            return out;
        }
        if(OpenClipboard(nullptr) == 0){
            return out;
        }
        HANDLE h = GetClipboardData(CF_HDROP);
        if(h != nullptr){
            HDROP drop = static_cast<HDROP>(GlobalLock(h));
            if(drop != nullptr){
                UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
                for(UINT i = 0; i < count; ++i){
                    UINT len = DragQueryFileW(drop, i, nullptr, 0);
                    std::wstring buf((size_t)len + 1, L'\0');
                    DragQueryFileW(drop, i, &buf[0], len + 1);
                    OmegaCommon::String s = wideToUtf8(buf.c_str(), (int)len);
                    out.push_back(OmegaCommon::FS::Path(s));
                }
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
        return out;
    }

    void setFilePaths(const OmegaCommon::Vector<OmegaCommon::FS::Path> & paths) override {
        if(OpenClipboard(nullptr) == 0){
            return;
        }
        EmptyClipboard();

        // CF_HDROP payload: a DROPFILES header followed by a
        // double-NUL-terminated list of wide paths.
        std::wstring list;
        for(const auto & path : paths){
            // str() is non-const; read it off a local copy.
            OmegaCommon::FS::Path p = path;
            list += utf8ToWide(p.str());
            list += L'\0';
        }
        list += L'\0';   // terminating empty string

        SIZE_T headerSize = sizeof(DROPFILES);
        SIZE_T dataBytes = list.size() * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, headerSize + dataBytes);
        if(h != nullptr){
            void *base = GlobalLock(h);
            if(base != nullptr){
                DROPFILES *df = static_cast<DROPFILES *>(base);
                df->pFiles = (DWORD)headerSize;   // offset to the path list
                df->pt.x = 0;
                df->pt.y = 0;
                df->fNC = FALSE;
                df->fWide = TRUE;                 // wide-char path list
                std::memcpy(static_cast<char *>(base) + headerSize,
                            list.data(), dataBytes);
                GlobalUnlock(h);
                if(SetClipboardData(CF_HDROP, h) == nullptr){
                    GlobalFree(h);
                }
            } else {
                GlobalFree(h);
            }
        }
        CloseClipboard();
    }

    void clear() override {
        if(OpenClipboard(nullptr) == 0){
            return;
        }
        EmptyClipboard();
        CloseClipboard();
    }

private:
    /// The registered "HTML Format" clipboard format id (CF_HTML has no
    /// predefined constant). Registered once, cached for the process.
    static UINT htmlFormat() {
        static UINT fmt = RegisterClipboardFormatW(L"HTML Format");
        return fmt;
    }
};

}

namespace OmegaWTK::Native {
    NativeClipboardPtr get_native_clipboard(){
        static NativeClipboardPtr instance = std::make_shared<Win::WinClipboard>();
        return instance;
    }
}
