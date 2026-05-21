#include "utils.h"
#include <cstdint>

#ifndef OMEGA_COMMON_FS_H
#define OMEGA_COMMON_FS_H

#if defined(_WIN32)
#define PATH_SLASH '\\'
#else 
#define PATH_SLASH '/'
#endif

namespace OmegaCommon::FS {

        /**
        @brief A platform specific filesystem path class.
        @paragraph 
        In addition to performing basic path parsing on construction and runtime, 
        it supports rapid string concatnation as well absolute path resolution.
        */
        class OMEGACOMMON_EXPORT Path {
            OmegaCommon::String _str;
            OmegaCommon::String _dir;
            OmegaCommon::String _fname;
            OmegaCommon::String _ext;

            bool isRelative;

            void parse(const OmegaCommon::String & str);
            using SELF = Path;
        public:
            // const unsigned getTokenCount(){ return tokens.size();};
            // String debugString(){
            //     std::ostringstream out;
            //     auto it = tokens.begin();
            //     while(it != tokens.end()){
            //         out << "{Type:" << int(it->type) << ",Content:" << it->str << "}, " << std::flush;
            //         ++it;
            //     };
            //     return out.str().c_str();
            // };
            /**
             Appends a CString to the end of the path.
             @param str 
             @returns Path
            */
            SELF & append(const char *str);

            /**
             @brief Appends a String to the end of the path.
             @param str 
             @returns Path
            */
            SELF & append(const OmegaCommon::String & str);

            /**
             @brief Appends a StrRef to the end of the path.
             @param str 
             @returns Path
            */
            SELF & append(const OmegaCommon::StrRef & str);

            /**
             Appends a CString to the end of the path.
             @param str 
             @returns Path
            */
            SELF & concat(const char *str);

            /**
             @brief Appends a String to the end of the path.
             @param str 
             @returns Path
            */
            SELF & concat(const OmegaCommon::String & str);

            /**
             @brief Appends a StrRef to the end of the path.
             @param str 
             @returns Path
            */
            SELF & concat(const OmegaCommon::StrRef & str);

            /// @name Concat Operators
            /// @{
            SELF operator+(const char *str);

            SELF operator+(const OmegaCommon::String & str);

             SELF operator+(const OmegaCommon::StrRef & str);
            /// @}
            /**
             Retrieve the path as a string. (Relative path) 
             @returns String &
            */
            OmegaCommon::String &str();

            /**
             @brief Retrieves the top directory part of the path (if it has one).
             @returns String
            */
            OmegaCommon::String & dir();

            /**
             @brief Retrieves the filename part of the path (if it has one).
             @returns String
            */
            OmegaCommon::String & filename();

            /**
             @brief Retrieves the file extension of the path (if it has one).
             @returns String
            */
            OmegaCommon::String & ext();

            /**
             @brief Gets the absolute path of this path
             @returns String
            */
            OmegaCommon::String absPath();

            /**
             @brief Returns the path string with platform-native separators.

             On Windows, every '/' in the stored path string is replaced
             with '\\'. On Unix-likes, the path is returned unchanged
             (forward slashes are native). Useful when handing a path
             string to APIs that do not accept the cross-platform
             forward-slash form (some Win32 / DWrite calls, error
             messages quoted to the user, …). Equivalent to `str()`
             on Unix; differs only on Windows.

             @returns String
            */
            OmegaCommon::String nativePath();
            bool exists();

            bool isFile();

            bool isDirectory();

            bool isSymLink();

            Path followSymlink();

            Path(const char * str);
            Path(const String & str);
            Path(StrRef & str);
            
            ~Path() = default;
        };

        inline bool exists(Path path){
            return path.exists();
        };

        // -- File I/O helpers --

        /// @brief Read entire text file into a String.
        OMEGACOMMON_EXPORT Result<String, StatusCode> readFile(Path path);

        /// @brief Read entire binary file into a byte vector.
        OMEGACOMMON_EXPORT Result<Vector<std::uint8_t>, StatusCode> readBinaryFile(Path path);

        /// @brief Write text contents to a file (creates or overwrites).
        OMEGACOMMON_EXPORT StatusCode writeFile(Path path, StrRef contents);

        /// @brief Write binary data to a file (creates or overwrites).
        OMEGACOMMON_EXPORT StatusCode writeBinaryFile(Path path, ArrayRef<std::uint8_t> data);

        // -- Copy / Move --

        /// @brief Copy a single file from src to dest.
        OMEGACOMMON_EXPORT StatusCode copyFile(Path src, Path dest);

        /// @brief Move (rename) a single file from src to dest. Falls back to copy+delete across filesystems.
        OMEGACOMMON_EXPORT StatusCode moveFile(Path src, Path dest);

        // -- Enumeration / Filtering --

        /// @brief Return all entries in dir whose filename matches a simple glob pattern (* and ? wildcards).
        OMEGACOMMON_EXPORT Vector<Path> glob(Path dir, StrRef pattern);

        OMEGACOMMON_EXPORT StatusCode changeCWD(Path newPath);

        OMEGACOMMON_EXPORT StatusCode createSymLink(Path  file,Path symlinkDest);

        OMEGACOMMON_EXPORT StatusCode createDirectory(Path path);

        OMEGACOMMON_EXPORT StatusCode deleteDirectory(Path path);

        class OMEGACOMMON_EXPORT DirectoryIterator {
            Path path;
            Path result_path;
            bool _end;
            using SELF = DirectoryIterator;
            void *data;
            #ifdef _WIN32
            void *dirp;
            #endif
            long loc;
        public:
            bool & end();
            explicit DirectoryIterator(Path path);
            Path operator*();
            SELF & operator++();
            ~DirectoryIterator();
        };
};

#endif