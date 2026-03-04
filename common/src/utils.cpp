#include "omega-common/common.h"
#include <cctype>

namespace OmegaCommon {

    Vector<String> split(StrRef s, char delim) {
        Vector<String> out;
        if (s.size() == 0) return out;
        const char *p = s.data();
        const char *end = p + s.size();
        const char *start = p;
        for (; p != end; ++p) {
            if (*p == delim) {
                if (p > start)
                    out.push_back(String(start, static_cast<size_t>(p - start)));
                start = p + 1;
            }
        }
        if (start < end)
            out.push_back(String(start, static_cast<size_t>(end - start)));
        return out;
    }

    String join(ArrayRef<StrRef> parts, StrRef sep) {
        String out;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) out.append(sep.data(), sep.size());
            out.append(parts[i].data(), parts[i].size());
        }
        return out;
    }

    static bool isSpace(char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    }

    StrRef trimRef(StrRef s) {
        if (s.size() == 0) return s;
        const char *beg = s.data();
        const char *end = beg + s.size();
        while (beg < end && isSpace(*beg)) ++beg;
        while (end > beg && isSpace(*(end - 1))) --end;
        return StrRef(beg, static_cast<StrRef::size_type>(end - beg));
    }

    String trim(StrRef s) {
        StrRef r = trimRef(s);
        return String(r.data(), r.size());
    }

    bool startsWith(StrRef s, StrRef prefix) {
        if (prefix.size() > s.size()) return false;
        return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin());
    }

    bool endsWith(StrRef s, StrRef suffix) {
        if (suffix.size() > s.size()) return false;
        return s.size() >= suffix.size() && std::equal(suffix.begin(), suffix.end(), s.end() - suffix.size());
    }

    String concat(ArrayRef<StrRef> parts) {
        size_t total = 0;
        for (size_t i = 0; i < parts.size(); ++i)
            total += parts[i].size();
        String out;
        out.reserve(total);
        for (size_t i = 0; i < parts.size(); ++i)
            out.append(parts[i].data(), parts[i].size());
        return out;
    }

    size_t hashValue(StrRef s) {
        size_t h = 0;
        for (StrRef::size_type i = 0; i < s.size(); ++i)
            h = h * 31u + static_cast<unsigned char>(s[i]);
        return h;
    }

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
