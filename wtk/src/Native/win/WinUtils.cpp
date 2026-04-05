#include "NativePrivate/win/WinUtils.h"

#include <unicode/ustring.h>

namespace OmegaWTK::Native {

    void cpp_str_to_cpp_wstr(OmegaCommon::String str,std::wstring & res){
        res.resize(str.size());
        int32_t len;
        UErrorCode err = U_ZERO_ERROR;
        u_strFromUTF8((UChar *)res.data(),res.size(),&len,str.c_str(),str.size(),&err);

    };

}
