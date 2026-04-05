#ifndef OMEGAWTK_CORE_UNICODE_H
#define OMEGAWTK_CORE_UNICODE_H

#include <cstdint>
#include <string>

#include "OmegaWTKExport.h"

namespace OmegaWTK {

    using UnicodeChar = char16_t;
    using Unicode32Char = char32_t;

    /**
     * A lightweight UTF-16 string wrapper used by the public OmegaWTK API.
     * ICU is kept in the implementation so including this header stays cheap.
     */
    class OMEGAWTK_EXPORT UniString {
        std::u16string data_;

        explicit UniString(std::u16string data);

    public:
        UniString() = default;
        explicit UniString(const char * utf8);

        static UniString fromUTF8(const char * utf8);
        static UniString fromUTF32(const Unicode32Char * utf32, std::int32_t length);

        std::int32_t length() const;
        const UnicodeChar * getBuffer() const;
    };

}

#endif
