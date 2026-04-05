#include "omegaWTK/Core/Unicode.h"

#include <unicode/ustring.h>

#include <cstring>
#include <vector>

namespace OmegaWTK {

namespace {

std::u16string utf16FromUChars(const UChar * buffer,std::int32_t length){
    if(buffer == nullptr || length <= 0){
        return {};
    }

    std::u16string out;
    out.resize(static_cast<size_t>(length));
    for(std::int32_t i = 0; i < length; ++i){
        out[static_cast<size_t>(i)] = static_cast<char16_t>(buffer[i]);
    }
    return out;
}

}

UniString::UniString(std::u16string data):data_(std::move(data)){
}

UniString::UniString(const char * utf8):data_(fromUTF8(utf8).data_){
}

UniString UniString::fromUTF8(const char * utf8){
    if(utf8 == nullptr){
        return {};
    }

    const auto srcLength = static_cast<int32_t>(std::strlen(utf8));
    if(srcLength == 0){
        return {};
    }

    UErrorCode err = U_ZERO_ERROR;
    int32_t utf16Length = 0;
    u_strFromUTF8(nullptr,0,&utf16Length,utf8,srcLength,&err);
    if(err != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(err)){
        return {};
    }

    std::vector<UChar> buffer(static_cast<size_t>(utf16Length));
    err = U_ZERO_ERROR;
    u_strFromUTF8(buffer.data(),utf16Length,&utf16Length,utf8,srcLength,&err);
    if(U_FAILURE(err)){
        return {};
    }

    return UniString(utf16FromUChars(buffer.data(),utf16Length));
}

UniString UniString::fromUTF32(const Unicode32Char * utf32, std::int32_t length){
    if(utf32 == nullptr || length <= 0){
        return {};
    }

    std::vector<UChar32> utf32Buffer(static_cast<size_t>(length));
    for(std::int32_t i = 0; i < length; ++i){
        utf32Buffer[static_cast<size_t>(i)] = static_cast<UChar32>(utf32[i]);
    }

    UErrorCode err = U_ZERO_ERROR;
    int32_t utf16Length = 0;
    u_strFromUTF32(nullptr,0,&utf16Length,utf32Buffer.data(),length,&err);
    if(err != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(err)){
        return {};
    }

    std::vector<UChar> buffer(static_cast<size_t>(utf16Length));
    err = U_ZERO_ERROR;
    u_strFromUTF32(buffer.data(),utf16Length,&utf16Length,utf32Buffer.data(),length,&err);
    if(U_FAILURE(err)){
        return {};
    }

    return UniString(utf16FromUChars(buffer.data(),utf16Length));
}

std::int32_t UniString::length() const {
    return static_cast<std::int32_t>(data_.size());
}

const UnicodeChar * UniString::getBuffer() const {
    return data_.c_str();
}

};
