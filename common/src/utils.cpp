#include "omega-common/common.h"

namespace OmegaCommon {

    String operator+(const String & lhs,const StrRef & rhs){
        String ret;
        ret = lhs;
        ret.resize(ret.size() + rhs.size());
        std::copy(rhs.begin(),rhs.end(),ret.begin());
        return ret;
    }
    WString operator+(const WString & lhs,const WStrRef & rhs){
        WString ret;
        ret = lhs;
        ret.resize(ret.size() + rhs.size());
        std::copy(rhs.begin(),rhs.end(),ret.begin());
        return ret;
    }
    UString operator+(const UString & lhs,const UStrRef & rhs){
        UString ret;
        ret = lhs;
        ret.resize(ret.size() + rhs.size());
        std::copy(rhs.begin(),rhs.end(),ret.begin());
        return ret;
    }

    void operator+=(String & lhs, StrRef & rhs){
        lhs.resize(lhs.size() + rhs.size());
        std::copy(rhs.begin(),rhs.end(),lhs.begin());
    }
    void operator+=(WString & lhs,WStrRef & rhs){
        lhs.resize(lhs.size() + rhs.size());
        std::copy(rhs.begin(),rhs.end(),lhs.begin());
    }
    void operator+=(UString & lhs,UStrRef & rhs){
        lhs.resize(lhs.size() + rhs.size());
        std::copy(rhs.begin(),rhs.end(),lhs.begin());
    }

    StrRef operator&(String & str){
        return str;
    }
    WStrRef operator&(WString & str){
        return str;
    }
    UStrRef operator&(UString & str){
        return str;
    }
    
    namespace Argv {
        
    }
    bool findProgramInPath(const StrRef & prog,String & out) {

        const char *path;
#ifdef _WIN32
        path = std::getenv("Path");
#else
        path = std::getenv("PATH");
#endif

        std::istringstream in(path);

        OmegaCommon::String str;
        while(!in.eof()) {
    #ifdef _WIN32
            std::getline(in,str,';');
    #else
            std::getline(in,str,':');
    #endif

            auto current_path = FS::Path(str).append(prog);
    #ifdef _WIN32
            current_path.concat(".exe");
    #endif
            if(current_path.exists()){
                if(current_path.isSymLink()){
                    out = current_path.followSymlink().str();
                }
                else {
                    out = current_path.str();
                }
                return true;
            }
        }
        return false;
    }

}
