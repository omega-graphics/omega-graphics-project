#include "NativePrivate/win/WinUtils.h"

#include "omegaWTK/Core/Unicode.h"

#include <Windows.h>
#include <atlstr.h>

namespace OmegaWTK::Native {

    void cpp_str_to_cpp_wstr(OmegaCommon::String str,std::wstring & res){
        res.resize(str.size());
        ATL::CStringW s(str.c_str());
        res = s;
    };

}
