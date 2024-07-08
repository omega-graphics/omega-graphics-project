#include "omega-common/fs.h"

#include <windows.h>
#include <shlwapi.h>
#include <PathCch.h>

#pragma comment(lib,"user32.lib")
#pragma comment(lib,"shlwapi.lib")

namespace OmegaCommon::FS {

    bool Path::isDirectory(){
        DWORD attrs = GetFileAttributes(str().c_str());
        return attrs & FILE_ATTRIBUTE_DIRECTORY;
    };

    bool Path::isFile(){
        DWORD attrs = GetFileAttributes(str().c_str());
        return attrs & FILE_ATTRIBUTE_NORMAL;
    };

    bool Path::isSymLink(){
        DWORD attrs = GetFileAttributes(str().c_str());
        return attrs & FILE_ATTRIBUTE_REPARSE_POINT;
    };

    Path Path::followSymlink() {
        DWORD attrs = GetFileAttributesA(str().c_str());
        HANDLE h = CreateFileA(_str.c_str(),GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
        DWORD bufferLen = GetFinalPathNameByHandleA(h,NULL,0,FILE_NAME_OPENED);
        auto pathBuf = new char[bufferLen];
        GetFinalPathNameByHandleA(h,pathBuf,bufferLen,FILE_NAME_OPENED);
        return {pathBuf};
    }

    bool Path::exists(){
        return PathFileExistsA(str().c_str());
    };

    OmegaCommon::String Path::absPath(){
        auto n_dir = _dir;
        for(auto & c : n_dir){
            if(c == '/')
                c = PATH_SLASH;
        };

        if(isRelative){
            CHAR buffer[MAX_PATH];
            GetCurrentDirectoryA(MAX_PATH,buffer);
            if(_dir.front() == '.')
                return buffer + n_dir.substr(1,n_dir.size()-1) + PATH_SLASH + _fname + "." + _ext;
            else 
                return std::string(buffer) + PATH_SLASH + n_dir + _fname + "." + _ext;

        }
        else {
            return n_dir + PATH_SLASH + _fname + "." + _ext;
        }
        
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