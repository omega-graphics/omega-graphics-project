
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
        // Resolve against the faithful original path string (`_str`), not
        // the parsed _dir/_fname/_ext tokens — the reconstruction
        // mis-resolved relative paths (only one leading '.' stripped, so
        // "../x" became CWD + "./x" without a separator; dot-less relative
        // inputs lost the dir/file separator; the allow-list dropped
        // characters). An absolute path is already absolute; a relative one
        // is CWD + '/' + the path, and the OS collapses '.'/'..' at lookup.
        if(_str.empty())
            return _str;
        if(_str.front() == '/')
            return _str;
        // Strip leading "./" segments so cwd-prepended paths don't end up
        // as "<cwd>/./<file>". POSIX file APIs collapse it at lookup time,
        // but it surfaces in error messages and downstream code that
        // hashes/compares the path string treats the two forms as
        // distinct. Mirrors the Win32 fix in fs-win.cpp. Repeated forms
        // like "././file" are handled by the loop; a lone "." yields a
        // bare-cwd path.
        std::string tail = _str;
        while(tail.size() >= 2 && tail[0] == '.' && tail[1] == '/'){
            tail.erase(0, 2);
        }
        if(tail == "."){
            tail.clear();
        }
        char cwd_buffer[PATH_MAX];
        getcwd(cwd_buffer,PATH_MAX);
        if(tail.empty())
            return std::string(cwd_buffer);
        return std::string(cwd_buffer) + PATH_SLASH + tail;
	}

	StatusCode changeCWD(Path newPath){
		chdir(newPath.absPath().c_str());
		return Ok;
	}

	Path getExecutablePath(){
		// /proc/self/exe is a symlink to the running binary on Linux.
		// readlink does not null-terminate, so terminate manually.
		char buf[PATH_MAX];
		ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
		if(len <= 0){
			return Path("");
		}
		buf[len] = '\0';
		return Path(OmegaCommon::String(buf, static_cast<size_t>(len)));
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
