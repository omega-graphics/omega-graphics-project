#include "omega-common/fs.h"

#include <windows.h>
#include <shlwapi.h>
#include <PathCch.h>

#pragma comment(lib,"user32.lib")
#pragma comment(lib,"shlwapi.lib")

namespace OmegaCommon::FS {

    bool Path::isDirectory(){
        DWORD attrs = GetFileAttributes(str().c_str());
        return bool(attrs & FILE_ATTRIBUTE_DIRECTORY);
    };

    bool Path::isFile(){
        DWORD attrs = GetFileAttributes(str().c_str());
        return bool(attrs & FILE_ATTRIBUTE_NORMAL);
    };

    bool Path::isSymLink(){
        DWORD attrs = GetFileAttributes(str().c_str());
        return bool(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
    };

    Path Path::followSymlink() {
        DWORD attrs = GetFileAttributesA(str().c_str());
        HANDLE h = CreateFileA(_str.c_str(),GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
        DWORD bufferLen = GetFinalPathNameByHandleA(h,NULL,0,FILE_NAME_OPENED);
        auto *pathBuf = new char[bufferLen];
        GetFinalPathNameByHandleA(h,pathBuf,bufferLen,FILE_NAME_OPENED);
        return {pathBuf};
    }

    bool Path::exists(){
        return bool(PathFileExistsA(str().c_str()));
    };

    OmegaCommon::String Path::absPath(){
        // Resolve against the faithful original path string (`_str`), not
        // the parsed _dir/_fname/_ext tokens. The reconstruction was lossy
        // and mis-resolved relative paths (only one leading '.' stripped,
        // so "../x" became CWD + "./x" with no separator; dot-less relative
        // inputs lost the dir/file separator; the ':' in a drive letter was
        // dropped, flipping isRelative). All slashes are normalized to '\\'
        // so the result is a native Windows path either way.
        auto normalize = [](OmegaCommon::String s) -> OmegaCommon::String {
            for(auto & c : s){
                if(c == '/')
                    c = PATH_SLASH;
            }
            return s;
        };
        if(_str.empty())
            return _str;
        // Absolute forms: drive letter ("C:\..."/"C:/..."), UNC / rooted
        // ("\\..." or "/...").
        bool driveAbs = _str.size() >= 3
                        && std::isalpha(static_cast<unsigned char>(_str[0]))
                        && _str[1] == ':' && (_str[2] == '/' || _str[2] == '\\');
        bool rootAbs = _str.front() == '/' || _str.front() == '\\';
        if(driveAbs || rootAbs)
            return normalize(_str);
        CHAR buffer[MAX_PATH];
        GetCurrentDirectoryA(MAX_PATH,buffer);
        return normalize(std::string(buffer) + PATH_SLASH + _str);
    };


    StatusCode createDirectory(Path path){
        BOOL success = CreateDirectoryA(path.str().c_str(),NULL);
        if(success){
            return Ok;
        }
        else {
            return Failed;
        };
    };

    StatusCode deleteDirectory(Path path){
        BOOL success = RemoveDirectoryA(path.str().c_str());
        if(success){
            return Ok;
        }
        else {
            return Failed;
        };
    };

    StatusCode createSymLink(Path file, Path symlinkDest){
        BOOL success = CreateSymbolicLinkA(symlinkDest.absPath().c_str(),file.absPath().c_str(),file.isDirectory()? SYMBOLIC_LINK_FLAG_DIRECTORY : NULL);
        if(success){
            return Ok;
        }
        else {
            return Failed;
        };
    };

    StatusCode changeCWD(Path newPath){
        MessageBoxA(GetForegroundWindow(),newPath.absPath().c_str(),"NOTE",MB_OK);
        BOOL success = SetCurrentDirectoryA(newPath.absPath().c_str());
        if(success){
            return Ok;
        }
        else {
            return Failed;
        };
    };

    Path getExecutablePath(){
        // GetModuleFileNameA returns the number of chars copied (excluding the
        // null terminator) and, when the path doesn't fit, returns the buffer
        // capacity with ERROR_INSUFFICIENT_BUFFER. Grow until it fits so long
        // paths aren't silently truncated.
        DWORD cap = MAX_PATH;
        for(;;){
            OmegaCommon::Vector<char> buf(cap);
            DWORD len = GetModuleFileNameA(NULL, buf.data(), cap);
            if(len == 0){
                return Path("");
            }
            if(len < cap){
                return Path(OmegaCommon::String(buf.data(), len));
            }
            cap *= 2;
            if(cap > 65536){ // sanity bound; no real exe path is this long
                return Path("");
            }
        }
    };


    DirectoryIterator::DirectoryIterator(Path path):path(path),result_path(""),_end(false){
        data = new WIN32_FIND_DATAA;
        HANDLE hFind;
        hFind = FindFirstFileA(path.str().c_str(),(LPWIN32_FIND_DATAA)data);
        if(hFind == INVALID_HANDLE_VALUE){
            /// Fuck!
        }
        dirp = hFind;
    };

    Path DirectoryIterator::operator*(){
        LPWIN32_FIND_DATAA d = (LPWIN32_FIND_DATAA)data;
        return path + d->cFileName;
    };

    DirectoryIterator & DirectoryIterator::operator++(){
        BOOL success = FindNextFileA(dirp,(LPWIN32_FIND_DATAA)data);
        if(!success)
            _end = true;
        return *this;
    };

    DirectoryIterator::~DirectoryIterator(){
        FindClose(dirp);
    }
};