
#include "omega-common/fs.h"

#include <dirent.h>
#include <linux/limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <cstring>



namespace OmegaCommon::FS {

	bool Path::isDirectory(){
		struct stat st = {0};
		auto p = absPath();
		if(stat(p.c_str(),&st) == -1){
			return false;
		}
		return S_ISDIR(st.st_mode);
	}

	bool Path::isFile(){
		struct stat st = {0};
		auto p = absPath();
		if(stat(p.c_str(),&st) == -1){
			return false;
		}
		return S_ISREG(st.st_mode);
	}

	String Path::absPath(){
		auto n_dir = _dir;
		std::string ending;
		if(_ext.empty()){
    		ending = _fname;
    	}
    	else {
    		ending = _fname + "." + _ext;
    	}
        // for(auto & c : n_dir){
        //     if(c == '/')
        //         c = PATH_SLASH;
        // };

        if(isRelative){
        	char cwd_buffer[PATH_MAX];
            getcwd(cwd_buffer,PATH_MAX);

            const char *buffer = cwd_buffer;
            if(_dir.front() == '.')
                return buffer + n_dir.substr(1,n_dir.size()-1) + PATH_SLASH + ending;
            else {

                return std::string(buffer) + PATH_SLASH + n_dir + ending;
            }

        }
        else {
            return n_dir + PATH_SLASH + _fname + "." + _ext;
        }
	}

	StatusCode changeCWD(Path newPath){
		chdir(newPath.absPath().c_str());
		return Ok;
	}

	bool Path::isSymLink(){
		struct stat st = {0};
		auto p = absPath();
		if(stat(p.c_str(),&st) == -1){
			return false;
		}
		else {
			return bool(st.st_mode & S_IFLNK);
		};
	}

	Path Path::followSymlink(){
		assert(isSymLink() && "The path is not a symlink!");
		auto p = absPath();
		OmegaCommon::String str;
		str.resize(PATH_MAX);
		auto n_size = readlink(p.c_str(),str.data(),PATH_MAX);
		str.resize(n_size);
		return {str};
	}

	bool Path::exists(){
		auto p = absPath();
		struct stat st = {0};
		return stat(p.c_str(),&st) != -1;
	}

	StatusCode createDirectory(Path path){
		auto p = path.absPath();

		struct stat st = {0};
		if(stat(p.c_str(),&st) == -1){
			mkdir(p.c_str(),ACCESSPERMS);
		}
		return Ok;
	}

	StatusCode deleteDirectory(Path path){
		rmdir(path.absPath().c_str());
		return Ok;
	}

	StatusCode createSymLink(Path file, Path symlinkDest){
		symlink(file.absPath().c_str(),symlinkDest.absPath().c_str());
		return Ok;
	}

	DirectoryIterator::DirectoryIterator(Path path):
	path(path),
	result_path(path),
	_end(false),
	data(nullptr),
	loc(-1){
		auto dir = opendir(path.absPath().c_str());
		data = dir;
		if(dir == nullptr){
			_end = true;
			return;
		}
		++(*this);
	}

	Path DirectoryIterator::operator*(){
		return result_path;
	}

	DirectoryIterator & DirectoryIterator::operator++(){
		auto *dir = static_cast<DIR *>(data);
		if(dir == nullptr){
			_end = true;
			return *this;
		}
		dirent *ent = nullptr;
		while((ent = readdir(dir)) != nullptr){
			if(std::strcmp(ent->d_name,".") == 0 || std::strcmp(ent->d_name,"..") == 0){
				continue;
			}
			result_path = path + ent->d_name;
			++loc;
			return *this;
		}
		_end = true;
		return *this;
	}

	DirectoryIterator::~DirectoryIterator(){
		auto *dir = static_cast<DIR *>(data);
		if(dir != nullptr){
			closedir(dir);
		}
	}
}
