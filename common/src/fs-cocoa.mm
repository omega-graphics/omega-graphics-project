#include "omega-common/fs.h"

#include <cstdio>
#include <unistd.h>
#include <dirent.h>

#import <Foundation/Foundation.h>

namespace OmegaCommon::FS { 

    bool Path::isDirectory(){
        NSFileManager *fileManager = [NSFileManager defaultManager];
        NSString *str = [[NSString alloc] initWithUTF8String:_str.c_str()];
        NSError *error;
        NSDictionary<NSFileAttributeKey,id> * dict = [fileManager attributesOfItemAtPath:str error:&error];
        NSFileAttributeType fileType = dict[NSFileType];
        return fileType == NSFileTypeDirectory;
    };

    bool Path::isFile(){
        NSFileManager *fileManager = [NSFileManager defaultManager];
        NSString *str = [[NSString alloc] initWithUTF8String:_str.c_str()];
        NSError *error;
        NSDictionary<NSFileAttributeKey,id> * dict = [fileManager attributesOfItemAtPath:str error:&error];
        NSFileAttributeType fileType = dict[NSFileType];
        return fileType == NSFileTypeRegular;
    };

    bool Path::isSymLink(){
        NSFileManager *fileManager = [NSFileManager defaultManager];
        NSString *str = [[NSString alloc] initWithUTF8String:_str.c_str()];
        NSError *error;
        NSDictionary<NSFileAttributeKey,id> * dict = [fileManager attributesOfItemAtPath:str error:&error];
        NSFileAttributeType fileType = dict[NSFileType];
        return fileType == NSFileTypeSymbolicLink;
    };

    Path Path::followSymlink() {
        NSFileManager *fileManager = [NSFileManager defaultManager];
        NSString *str = [[NSString alloc] initWithUTF8String:_str.c_str()];
        NSError *error;
        NSString *returnPath = [fileManager destinationOfSymbolicLinkAtPath:str error:&error];
        if(error.code >= 0){
            return *this;
        }
        else {
            return {returnPath.UTF8String};
        }
    }

    bool Path::exists(){
        NSFileManager *fileManager = [NSFileManager defaultManager];
        BOOL rc = [fileManager fileExistsAtPath:[[NSString alloc] initWithUTF8String:absPath().c_str()]];
        return rc == YES;
    };


    String Path::absPath(){
        auto n_dir = _dir;
        // for(auto & c : n_dir){
        //     if(c == '/')
        //         c = PATH_SLASH;
        // };

        if(isRelative){
            NSString *currentDir = [[NSFileManager defaultManager] currentDirectoryPath];
            const char *buffer = currentDir.UTF8String;
            if(_dir.front() == '.')
                return buffer + n_dir.substr(1,n_dir.size()-1) + PATH_SLASH + _fname + "." + _ext;
            else 
                return std::string(buffer) + PATH_SLASH + n_dir + _fname + "." + _ext;

        }
        else {
            return n_dir + PATH_SLASH + _fname + "." + _ext;
        }
        
    };


    StatusCode changeCWD(Path newPath){
        chdir(newPath.str().c_str());
        return Ok;
    };

    StatusCode createSymLink(Path file, Path symlinkDest){
        NSFileManager *fileManager = [NSFileManager defaultManager];
        NSURL *src = [NSURL fileURLWithFileSystemRepresentation:file.str().c_str() isDirectory:YES relativeToURL:nil];
        NSURL *dest = [NSURL fileURLWithFileSystemRepresentation:symlinkDest.str().c_str() isDirectory:YES relativeToURL:nil];
        NSError *error;
        [fileManager createSymbolicLinkAtURL:src withDestinationURL:dest error:&error];
        if(error.code >= 0){
            return Ok;
        }
        else {
            return Failed;
        };
    };

    StatusCode createDirectory(Path path){
        NSFileManager *fileManager = [NSFileManager defaultManager];
        NSURL *fileUrl = [NSURL fileURLWithFileSystemRepresentation:path.str().c_str() isDirectory:YES relativeToURL:nil];
        NSError *error;
        [fileManager createDirectoryAtURL:fileUrl withIntermediateDirectories:YES attributes:nil error:&error];
//        if(error.domain == nil) {
//            return Ok;
//        }
//        if(error.code >= 0){
//            return Ok;
//        }
//        else {
//            return Failed;
//        };
        return Ok;
    };

    StatusCode deleteDirectory(Path path){
        NSFileManager *fileManager = [NSFileManager defaultManager];
        NSURL *fileUrl = [NSURL fileURLWithFileSystemRepresentation:path.str().c_str() isDirectory:YES relativeToURL:nil];
        NSError *error;
        [fileManager removeItemAtURL:fileUrl error:&error];
        if(error.code >= 0){
            return Ok;
        }
        else {
            return Failed;
        };
    };

    DirectoryIterator::DirectoryIterator(Path path):path(path),result_path(""){
        DIR * dir = opendir(path.str().c_str());
        data = dir;
    };

    DirectoryIterator & DirectoryIterator::operator++(){
        DIR *dir = (DIR *)data;
        ++loc;
        dirent *ent;
        if((ent = readdir(dir)) == NULL) {
            _end = true;
            result_path = "";
        }
        else 
        {
            StrRef view(ent->d_name, ent->d_namlen);
            result_path = path + "/" + view;
        }
        return *this;
    };

    Path DirectoryIterator::operator*(){
        return result_path;
    };
    
};