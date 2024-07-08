#include "utils.h"

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
            typedef Path SELF;
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

inline OmegaCommon::FS::Path operator "" _FP(const char *path){
    return {path};
}

#endif